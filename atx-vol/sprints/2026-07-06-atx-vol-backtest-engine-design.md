# atx-vol — Backtest Engine Design & Implementation Plan

**Date:** 2026-07-06
**Status:** Design of record → implementation.
**Scope:** `atx-vol`. Build the strategy + backtest layer (originally sprint
`2026-07-05` P4-2/P4-3, deferred) out into a **general, high-performance option
analytics backtesting engine** — strategy-agnostic, declarative-strategy driven,
integrated seamlessly on the existing PricedSurface / SurfaceArchive / corpus /
PortfolioPricer spine. No new pricing math.

This follows and consumes the unification sprint's shipped foundation: P0
(American greeks FD path, PnL dvol-at-same-T), P1 (PricedSurface-native portfolio
unification), P2 (OPRA ingest at scale), P3 (surface-archive corpus + manifest),
P4-1 (`DispersionBook` — vega-neutral straddle + implied-correlation signal).

## Locked decisions (brainstorm, 2026-07-06)

| Decision | Choice |
|---|---|
| Engine scope | **General framework.** Strategy is an interface; engine (loop, MTM, attribution, frictions, risk overlay, metrics) is strategy-agnostic. `DispersionBook` is the first strategy. |
| Time model | **EOD daily-stepped, event-driven-ready core.** Walk corpus dates (one archive/date = one snapshot). `Clock` abstraction keys off timestamp, not "date," so intraday snapshots slot in later with no re-architecture. |
| SOTA pillars | All four: **analytics/attribution depth · execution realism/frictions · throughput/determinism · research ergonomics**. |
| Strategies | **Declarative leg-template DSL.** Strategies are *data* (`StrategySpec`) resolved by the engine, not hardcoded. A custom `IStrategy` remains the escape hatch. |
| Financing | **Full cash + borrow ledger now** (premium financing at `r`, short-share borrow, shares carry via `q_eff`, NAV reconciliation). |
| Metrics | Standard tearsheet **plus vega-scaled / per-unit-risk** metrics. |
| Hedging | Engine-owned configurable delta-hedge overlay (`HedgePolicy`), cadence daily or at-roll — reusable, not per-strategy. |
| Spread | Modeled (surfaces are fitted **mid**): `fill = mark ± half_spread`, documented as a model, zero-friction reproduces frictionless bit-for-bit. |

---

## Baseline — what already exists (the spine)

The backtester is an orchestration layer over primitives that are all built,
tested, and green (642/642 at sprint P4-1). Nothing below is invented here.

| Seam | API | Role in the engine |
|---|---|---|
| Corpus index | `read_manifest_file(path) → CorpusManifest` (`dates`, `entries{date,symbol,archive_path,...}`) | enumerate the backtest timeline + per-date archive locations |
| Load | `SurfaceArchive::open_file(path) → Result<SurfaceArchive>`; `.map_all() → Result<vector<PricedSurface>>`; `.directory()` | load one date's surfaces (owned) |
| Resolve | `SurfaceSet::create(span<const PricedSurface* const>) → Result<SurfaceSet>`; `.find(uid)` | uid → surface per underlying |
| Reprice | `PricedSurface::{iv,fair_value,greeks,forward_at,q_eff_at}(K,T[,side])` | mark + American greeks at arbitrary (K,T) within domain |
| Price book | `PortfolioPricer::price(set) → PriceFrame` (PV + 9 greeks, position-scaled, SoA) | daily MTM + book greeks |
| Attribute | `PortfolioPricer::pnl_explain(base_set, shifted_set) → PnlFrame` | **Taylor attribution; ages T by the two surfaces' `now_ts_ns` gap internally** (the theta/charm engine) |
| Remap | `with_uid(const PricedSurface&, uid) → Result<PricedSurface>` | synthetic/relabelled surfaces (tests) |
| Strategy #1 | `build_dispersion_book(universe, set, cfg) → DispersionBook{signal, legs, positions}` | first strategy, re-expressed as a `StrategySpec` |

Key convention (from `portfolio_pricer.hpp`): `kNsPerYear = 365.25*86400*1e9`;
greeks are **American, spot-based, position-scaled**; `pnl_explain` decomposes
the full American PnL, `pnl_unexplained` is the higher-order Taylor tail (spot-only
step ⇒ ~1e-4 relative, gated in P0).

---

## Architecture

### Layers (bottom-up)

```
  ┌─────────────────────────────────────────────────────────────┐
  │ Results:  BacktestResult (SoA time series) · TearSheet · TSV  │
  ├─────────────────────────────────────────────────────────────┤
  │ Engine:   Clock · run_backtest  (load→MTM→strategy→hedge→     │
  │           fill→accrue→record), sequential over dates          │
  ├───────────────────────────┬─────────────────────────────────┤
  │ Strategy interpreter:      │ Execution + accounting:          │
  │  StrategySpec DSL →         │  FrictionModel (fills, costs),   │
  │  concrete target Lots       │  CashLedger (financing/borrow),  │
  │  (strike-from-δ, tenor,     │  hedge overlay (exec HedgeSpec)  │
  │  structure, sizing,         │                                  │
  │  cross-leg constraints)     │                                  │
  ├───────────────────────────┴─────────────────────────────────┤
  │ Book state: PortfolioState · Lot (absolute-expiry anchor)     │
  ├─────────────────────────────────────────────────────────────┤
  │ Snapshot:   MarketSnapshot (owned surfaces + SurfaceSet + ts) │
  ├─────────────────────────────────────────────────────────────┤
  │ Data (exists): CorpusManifest · SurfaceArchive · PricedSurface│
  └─────────────────────────────────────────────────────────────┘
```

### Event-driven-ready core

`Clock` yields a sequence of `SnapshotRef{ ts_ns, date, archive_path }`. For the
corpus that is one per date (EOD). Every engine handler is written against
`now_ts_ns`, never a date index, so an intraday clock (multiple snapshots/day)
requires no engine change — only a richer `Clock` and per-snapshot archives. The
date loop is **sequential** (PnL is path-dependent); parallelism lives *inside* a
snapshot (the `PortfolioPricer` fan-out, already bit-deterministic) and *across*
independent backtests (param sweeps).

---

## The Strategy DSL (centerpiece)

A strategy is **declarative data**: a set of leg templates + cross-leg sizing
constraint + lifecycle + hedge policy. The engine's *interpreter* resolves a
`StrategySpec` against each snapshot into concrete `Lot`s. This is what makes
"3m 25Δ put, delta-hedged daily, new clip each day" and "9m 40Δ strangle XOM vs
3m 40Δ strangle SPY, flat vega" expressible without bespoke code.

### Grammar

```cpp
// include/atx/vol/strategy.hpp

// WHICH tenor: a target year-fraction, optionally snapped to a listed slice T.
struct TenorSpec {
  double target_T{30.0/365.25};       // e.g. 0.25 (3m), 0.75 (9m)
  bool   snap_to_listed{false};       // false: price on the interpolated surface at target_T
};                                    // true: resolve to the nearest surface slice T (hook; B1 = false)

// WHICH strike, per leg-side.
struct StrikeSelector {
  enum class Kind : std::uint8_t { AtmForward=0, Delta=1, Moneyness=2, AbsStrike=3 };
  Kind   kind{Kind::AtmForward};
  double value{0.0};                  // Delta: target |delta| (0.25); Moneyness: k=ln(K/F); AbsStrike: K
};

// WHAT structure. Each expands to 1..N option legs at resolved strikes.
struct StructureSpec {
  enum class Kind : std::uint8_t { Single=0, Straddle=1, Strangle=2, RiskReversal=3 };
  Kind kind{Kind::Straddle};
  Side single_side{Side::Call};       // Single only
  StrikeSelector call_leg{};          // Strangle/RR: OTM call selector (e.g. Delta 0.40)
  StrikeSelector put_leg{};           // Strangle/RR: OTM put  selector (e.g. Delta 0.40)
};

// HOW MUCH. Resolved after strikes are known (needs per-leg vega).
struct SizeSpec {
  enum class Kind : std::uint8_t { FixedContracts=0, TargetVega=1, Weight=2 };
  Kind   kind{Kind::TargetVega};
  double value{10'000.0};             // contracts, target vega, or a relative weight
  double sign{+1.0};                  // +1 long / -1 short the structure
};

// ONE leg-template on ONE underlier.
struct LegSpec {
  std::string   symbol;               // resolved to uid against the snapshot's SurfaceSet
  std::uint32_t uid{0};
  TenorSpec     tenor{};
  StructureSpec structure{};
  StrikeSelector strike{};            // Single/Straddle strike; Strangle uses structure.{call,put}_leg
  SizeSpec      size{};
  std::string   group;                // cross-leg constraint group tag (e.g. "index"/"basket", "long"/"short")
};

// CROSS-LEG sizing constraint applied AFTER per-leg base sizing.
struct CrossLegConstraint {
  enum class Kind : std::uint8_t { None=0, FlatVega=1, VegaNeutralBasket=2 };
  Kind kind{Kind::None};
  std::string group_a;                // FlatVega: scale group_b so Σvega(b) = Σvega(a)
  std::string group_b;                // VegaNeutralBasket: scale "basket" so Σvega = vega("index")
};

// LIFECYCLE: when to enter, how long to hold.
struct LifecycleSpec {
  enum class Entry : std::uint8_t { EveryStep=0, EveryNDays=1 };
  enum class Holding : std::uint8_t { HoldToExpiry=0, RollAtHorizon=1 };
  Entry   entry{Entry::EveryNDays};
  Holding holding{Holding::RollAtHorizon};
  unsigned entry_every_n{21};         // EveryNDays cadence (trading steps)
  // HoldToExpiry => overlapping clips (a new cohort each entry, each aged to its
  // own expiry, auto-closed at T<=0). RollAtHorizon => single book, closed+reopened
  // when the front cohort's residual T falls below `roll_at_T`.
  double  roll_at_T{7.0/365.25};
};

// HEDGE overlay (engine-owned, configurable).
struct HedgeSpec {
  enum class Kind : std::uint8_t { None=0, DeltaToZero=1 };
  enum class Cadence : std::uint8_t { AtEntry=0, Daily=1 };
  Kind    kind{Kind::None};
  Cadence cadence{Cadence::Daily};
  double  band{0.0};                  // rebalance only when |net delta| > band (0 = every cadence tick)
};

struct StrategySpec {
  std::string        name;
  std::vector<LegSpec> legs;
  CrossLegConstraint constraint{};
  LifecycleSpec      lifecycle{};
  HedgeSpec          hedge{};
};
```

### The two worked examples, as specs

**A. 3-month 25Δ put, delta-hedged daily, new clip each day, held to expiration**

```cpp
StrategySpec put_clip {
  .name = "spy-3m-25d-put-daily-clip",
  .legs = {{
    .symbol="SPY", .tenor={.target_T=0.25},
    .structure={.kind=Single, .single_side=Side::Put},
    .strike={.kind=StrikeSelector::Kind::Delta, .value=0.25},
    .size={.kind=SizeSpec::Kind::FixedContracts, .value=1.0, .sign=+1.0},
  }},
  .lifecycle={.entry=EveryStep, .holding=HoldToExpiry},   // overlapping clips
  .hedge={.kind=DeltaToZero, .cadence=Daily},
};
```

**B. Buy 9m 40Δ strangle XOM, sell 3m 40Δ strangle SPY, flat vega**

```cpp
StrategySpec cross_strangle {
  .name = "xom9m-vs-spy3m-40d-strangle-flat-vega",
  .legs = {
    { .symbol="XOM", .tenor={.target_T=0.75},
      .structure={.kind=Strangle,
                  .call_leg={.kind=Delta,.value=0.40}, .put_leg={.kind=Delta,.value=0.40}},
      .size={.kind=Weight, .value=1.0, .sign=+1.0}, .group="a" },
    { .symbol="SPY", .tenor={.target_T=0.25},
      .structure={.kind=Strangle,
                  .call_leg={.kind=Delta,.value=0.40}, .put_leg={.kind=Delta,.value=0.40}},
      .size={.kind=Weight, .value=1.0, .sign=-1.0}, .group="b" },
  },
  .constraint={.kind=FlatVega, .group_a="a", .group_b="b"},   // scale SPY so |vega|=XOM |vega|
  .lifecycle={.entry=EveryNDays, .holding=RollAtHorizon, .entry_every_n=21},
  .hedge={.kind=None},
};
```

**C. Dispersion (P4-1) as a spec** — index ATM straddle (short) + basket ATM
straddles (long, per-name weight), `VegaNeutralBasket` constraint, roll cadence.
`build_dispersion_book` becomes a thin adapter that builds this spec and also
emits the implied-correlation signal via the strategy signal hook (below), so the
existing `DispersionBook` API and tests are preserved.

### Resolution primitives (engine-internal)

```cpp
// src/strategy.cpp

// Root-find the strike whose American |delta| equals the target, on a
// log-moneyness bracket around F(T). |delta|(K) is monotone per side
// (call |delta| ↓ in K, put |delta| ↑ in K), so bisection on the bracket
// converges deterministically (fixed rel-tol + iteration cap).
// @return InvalidArgument if the target is unreachable within the surface domain.
Result<double> resolve_strike_by_delta(const PricedSurface&, double T, Side,
                                       double target_abs_delta);

// Resolve a StrikeSelector to an absolute K (AtmForward=F(T); Delta=solver;
// Moneyness=F·e^k; AbsStrike passthrough).
Result<double> resolve_strike(const PricedSurface&, const TenorSpec&,
                              Side, const StrikeSelector&);

// Expand a LegSpec against a snapshot into concrete (K,T,side) legs with per-leg
// straddle/strangle vega, before sizing. Single→1, Straddle/Strangle→2, RR→2.
struct ResolvedLeg { std::uint32_t uid; double K, T, sigma, vega; Side side; std::string group; };
Result<std::vector<ResolvedLeg>> expand_leg(const MarketSnapshot&, const LegSpec&);
```

Sizing pipeline: (1) `expand_leg` → resolved legs with vega; (2) apply per-leg
`SizeSpec` (FixedContracts direct; TargetVega ⇒ `qty = sign·target/(vega·mult)`;
Weight ⇒ base qty = sign·weight, unitless pre-constraint); (3) apply
`CrossLegConstraint` (FlatVega ⇒ scale group_b so `Σ|qty·vega·mult|` matches
group_a; VegaNeutralBasket ⇒ scale basket group to the index leg's vega — the
exact P4-1 mechanic, weights normalized); (4) emit `Lot`s.

---

## Engine mechanics

### Snapshot loader

```cpp
// src/backtest.cpp
class MarketSnapshot {
 public:
  static Result<MarketSnapshot> load(std::string_view archive_path, std::int64_t ts_ns);
  const SurfaceSet& set() const noexcept;
  const PricedSurface* find(std::uint32_t uid) const noexcept;
  std::int64_t ts_ns() const noexcept;
  std::optional<std::uint32_t> uid_of(std::string_view symbol) const;   // via directory()
 private:
  std::vector<PricedSurface> surfaces_;   // owned (map_all)
  SurfaceSet set_;                        // non-owning over surfaces_ (stable addresses)
};
```

The engine holds exactly **two live snapshots** — `base_` and `shifted_` — so
peak memory is two dates, never the whole corpus. `ts_ns` is taken from the
surfaces' `PricingContext.now_ts_ns` (they agree within a date).

**Load-once invariant (explicit operator requirement).** Each date's archive is
opened from disk **exactly once**. After a step's PnL is computed, the engine does
`base_ = std::move(shifted_)` — the just-loaded shifted snapshot *becomes* the
next base with no re-open, no re-`map_all`, no re-`SurfaceSet` build — and only
the following date is loaded fresh into `shifted_`. Over an N-date backtest there
are exactly N `open_file`/`map_all` calls (asserted in the B0 determinism gate:
archive-open count == N). `MarketSnapshot` is move-only. The move-swap is
pointer-safe by construction: `std::vector` move transfers the heap buffer without
reallocating, so element addresses are stable and `set_`'s `const PricedSurface*`
into `surfaces_` stay valid after `base_ = std::move(shifted_)` — provided
`surfaces_` is never mutated after `set_` is built (it is not; the loader finalizes
`surfaces_` first, builds `set_` over it, then returns a fully-seated snapshot).
No re-seating needed.

### Book state & aging

```cpp
struct Lot {
  std::uint64_t id;          // stable engine key
  OptionContract contract;   // {uid,K,T,side}; T re-aged each step
  double qty, multiplier;
  std::int64_t expiry_ts_ns; // absolute anchor: T_now = (expiry_ts - now_ts)/kNsPerYear
  std::uint32_t cohort;      // clip/cohort tag (overlapping HoldToExpiry lifecycle)
  double entry_price;        // per-share fill (realized-cost / roll accounting)
};

class PortfolioState {
  std::vector<Lot> lots_;    // all open lots across all cohorts
  double shares_[/*per uid*/];  // delta-hedge shares ledger (marked at spot)
  // + CashLedger (below)
};
```

**Aging is delegated to `pnl_explain`.** For step base→shifted the engine builds
a `Portfolio` whose contract `T`s are the **base-date** residuals; `pnl_explain`
rolls them by the surfaces' `now_ts_ns` gap to the shifted date (theta/charm),
repricing the shifted leg at `T−dt`. Because each lot's `expiry_ts_ns` is fixed,
`T_base − dt == T_shifted` exactly — no drift. After the step the engine adopts
shifted-date residual T's for the next step. Lots with `T ≤ 0` are settled at
intrinsic and removed (HoldToExpiry cohorts self-terminate this way).

### Engine loop (canonical — resolve today, price forward, move-swap)

The loop is a single forward pass. Positions are **resolved on today's (base)
snapshot** (which is already in memory — on the first date it was just loaded, on
every later date it is the previous iteration's shifted, moved in), then priced
**forward to tomorrow (shifted, the one date loaded this iteration)** via
`pnl_explain`. Then shifted becomes base by move, and the next date is loaded.
Each archive is opened exactly once. A 3-date corpus `[t0,t1,t2]` runs as:

```
base    = MarketSnapshot::load(dates[0])          // t0 — 1st and only load of t0
book    = strategy.step(base, {}, ledger)         // resolve/open positions AS OF t0
for i in 1 .. N-1:
    shifted = MarketSnapshot::load(dates[i])       // the ONLY disk load this iteration
    // PnL of today's book, today (base) -> tomorrow (shifted):
    frame = pricer(book @ base-T).pnl_explain(base.set, shifted.set)   // ages T by ts gap
    accrue = ledger.step(base, shifted)            // financing/borrow/shares carry over dt
    record(dates[i], frame, accrue, book_greeks, signals)   // store @ granularity
    base = std::move(shifted)                       // SWAP — t_i is now base, no reload
    book = strategy.step(base, book, ledger)        // resolve AS OF t_i: entries/rolls/hedge/fills
// loop ends when dates exhausted -> N-1 priced steps (+ inception row 0)
```

`strategy.step` is where entries, rolls, the delta-hedge overlay, and fills happen
(all on the base snapshot, at base-date marks); `pnl_explain` the next iteration
prices that book forward. So a position opened at `t_i` is filled at `t_i` prices
and its first MTM is the `t_i → t_{i+1}` step — exactly the "resolve today, price
between today and tomorrow, swap, repeat" cadence.

**One `strategy.step` breakdown** (base snapshot `S`, prior book `B`, ledger `L`):
1. **Signals:** evaluate strategy diagnostics on `S` (dispersion ⇒ implied corr).
2. **Target:** interpreter resolves `StrategySpec` against `S` → desired lots
   (entries per `LifecycleSpec`: overlapping clip cohorts, or a roll of the front
   cohort). `None`/hold ⇒ keep `B`.
3. **Hedge overlay:** if `HedgeSpec` fires this cadence, net book delta (option
   delta on `S` + existing shares) → trade shares to `|net delta| ≤ band`.
4. **Execute:** diff `B` vs target → trades; `FrictionModel` fills (bid/ask,
   per-contract cost, slippage); `L` books premium in/out + realized frictions.
5. Return the new book (aged to `S`'s date for the next `pnl_explain`).

### Storage granularity

`RunConfig` carries a record-granularity knob so the output series is stored at
the operator's chosen resolution, decoupled from the (daily) step cadence:

```cpp
struct RunConfig {
  PriceOptions   price{};            // pricer thread fan-out (bit-deterministic)
  FrictionModel  frictions{};
  FinancingConfig financing{};
  unsigned       record_every_n{1};  // persist every Nth step (1 = every step)
  bool           retain_position_frames{false};  // keep per-position PnlFrame per recorded step
                                                 // (default: aggregate totals + book greeks only)
};
```

Steps between recorded rows still fully process (PnL accrues into the running NAV
and the next recorded row's cumulative columns) — `record_every_n` only downsamples
what is *persisted*, so a coarse-granularity run and a fine one agree on cumulative
NAV/attribution at the coarse sample points.

### Cash + borrow ledger (full)

```cpp
struct FinancingConfig {
  double borrow_rate{0.0};   // continuous, on |short shares|·S (hard-borrow proxy)
  bool   finance_premium{true};   // option premium balance accrues at r
  bool   shares_carry{true};      // long shares earn q_eff·S·dt, pay r·S·dt (cost of carry)
};
class CashLedger {
  double cash_{0.0};
  // daily: cash_ *= exp(r·dt); minus borrow_rate·Σ|short_i|·S_i·dt;
  //        shares carry via q_eff (long earns div, pays finance).
  // trade: cash_ += Σ (sell premium − buy premium − frictions).
};
```

**NAV reconciliation (a gate):** `NAV_d = cash_d + Σ lot.qty·mult·mark_d +
Σ shares·S_d`, where `mark_d = PricedSurface::fair_value` (mid). `PnL_d = NAV_d −
NAV_{d-1}` must equal `Σ pnl_explain axes + shares PnL + financing` to Taylor
tolerance. Spread/costs are realized only on trades (they hit `cash` at fill), so
frictionless config ⇒ `NAV = cash + book mark` with zero realized spread.

---

## Frictions model (honest, documented)

PricedSurface is a fitted **mid** surface — no stored bid/ask — so the spread is
*modeled*:

```cpp
struct FrictionModel {
  enum class SpreadKind : std::uint8_t { None=0, PriceBps=1, VolTicks=2 };
  SpreadKind spread_kind{SpreadKind::None};
  double half_spread_bps{0.0};       // PriceBps: fill = mark·(1 ± bps/1e4)
  double vol_tick{0.0};              // VolTicks: half_spread = vega·vol_tick (per share)
  double per_contract_cost{0.0};     // $ per contract traded
  double hedge_slippage_bps{0.0};    // shares fill at S·(1 ± bps/1e4)
  // Applied ONLY on traded quantity at entry/roll/hedge; holding accrues financing only.
};
```

`SpreadKind::None` + zero costs ⇒ fills at mark ⇒ **bit-identical** to the
frictionless run (gate). Drag is monotonic increasing in `half_spread_bps` (gate).

---

## Results & metrics

### BacktestResult (SoA time series, reserved to n_steps)

```cpp
struct BacktestResult {
  std::vector<std::string>  date;
  std::vector<std::int64_t> ts_ns;
  std::vector<double> pnl_total, pnl_delta, pnl_gamma, pnl_vega, pnl_vanna,
                      pnl_volga, pnl_theta, pnl_rho, pnl_charm, pnl_unexplained;
  std::vector<double> pnl_shares, financing, cost;      // hedge PnL, carry, realized frictions
  std::vector<double> nav, cash;
  std::vector<double> gross_delta, gross_gamma, gross_vega, gross_theta;  // book greeks EOD
  std::vector<double> turnover_notional, turnover_vega; // traded |notional| / |vega| this step
  std::vector<double> n_open_lots;
  std::vector<std::pair<std::string, std::vector<double>>> signals;  // strategy diagnostics (impl corr, …)
};
```

### TearSheet (pure function of BacktestResult)

```cpp
struct TearSheet {
  // Standard
  double total_return, ann_return, ann_vol, sharpe, max_drawdown, hit_rate,
         avg_turnover, total_cost, total_financing;
  // Attribution totals (Σ over steps; must close to total_return within tol)
  double attr_delta, attr_gamma, attr_vega, attr_vanna, attr_volga, attr_theta,
         attr_rho, attr_charm, attr_unexplained, attr_shares;
  // Vega-scaled / per-unit-risk  ← requested
  double return_on_gross_vega;   // Σpnl / mean(gross_vega)
  double vega_adj_sharpe;        // mean(pnl/gross_vega_prev) / std(...) · √252
  double pnl_per_vega_traded;    // Σpnl / Σturnover_vega
  double avg_gross_vega, avg_gross_gamma;
};
TearSheet tearsheet(const BacktestResult&, double periods_per_year = 252.0);

Status write_backtest_tsv(const BacktestResult&, std::string_view path);   // deterministic TSV
```

---

## Determinism & throughput

- Date loop sequential (path-dependent). Parallelism inside a snapshot via
  `PortfolioPricer` fan-out (bit-deterministic across thread counts) + across
  independent backtests.
- Gate: run at `n_threads=1` vs `N` ⇒ every `BacktestResult` vector `memcmp`-equal.
- Strike-from-delta solver iterates to a fixed relative tolerance with a fixed
  iteration cap ⇒ same K every run. All accumulation in input/date order (no
  float-add reordering).
- Throughput bench: steps/s over a synthetic corpus; the reprice dominates, the
  engine adds only book diffing + ledger arithmetic.

---

## New files (cumulative)

- `include/atx/vol/strategy.hpp` — the DSL (`StrategySpec` et al.), `IStrategy`,
  `StrategyContext/Decision`, resolution primitive decls.
- `src/strategy.cpp` — interpreter: `resolve_strike_by_delta`, `expand_leg`,
  sizing, cross-leg constraints.
- `include/atx/vol/backtest.hpp` + `src/backtest.cpp` — `Clock`, `MarketSnapshot`,
  `Lot`/`PortfolioState`, `CashLedger`, `FrictionModel`, `HedgePolicy`,
  `BacktestResult`, `TearSheet`, `run_backtest`.
- `src/dispersion_strategy.cpp` — `DispersionStrategy` (spec adapter + impl-corr
  signal); `build_dispersion_book` retained, re-expressed atop the spec.
- `tests/strategy_test.cpp`, `tests/backtest_test.cpp` — gate tests.
- `examples/dispersion_backtest.cpp`, `examples/strategy_examples.cpp` — the
  worked examples on a synthetic corpus.
- `CMakeLists.txt` / `tests/CMakeLists.txt` — register the above.

---

## Implementation phasing

Each phase independently shippable, TDD (gate test first), subagent-driven
(orchestrate + independently rebuild/re-run the suite), explicit-path commit
(never `git add -A`; the concurrent pf3/docs workstream stays untouched).

### B0 — Snapshot loader + engine skeleton + MTM/attribution

**Build.** `MarketSnapshot::load` (open_file → map_all → SurfaceSet over stable
addresses, move-only, fully-seated); `Clock` over a `CorpusManifest`;
`Lot`/`PortfolioState` with absolute-expiry aging; the canonical forward-pass loop
(resolve-today → `pnl_explain`-forward → `base_ = std::move(shifted_)` → load next)
that MTMs a **fixed hand-built book** (no strategy, no frictions); `RunConfig`
(record granularity) + `BacktestResult` populated with PnL + attribution + book
greeks. Driver holds one book to expiry.

**Gate tests** (`backtest_test.cpp`):
- *Load-once:* a spy'd loader (open counter) over an N-date corpus ⇒ exactly N
  archive opens (the move-swap never reloads a base).
- *Aging:* a two-date synthetic corpus where d+1 differs from d by **spot only**
  ⇒ `pnl_unexplained ≈ 0` (P0 tolerance); by **time only** ⇒ PnL isolates to theta.
- *Attribution closes:* `Σ axes + unexplained == pnl_total` per step (exact).
- *Determinism:* `n_threads` 1 vs N ⇒ `BacktestResult` `memcmp`-identical.
- *Granularity:* `record_every_n=k` ⇒ persisted cumulative NAV/attribution at the
  sampled steps equals the `record_every_n=1` run's values at those same steps.
- *Expiry settlement:* a lot crossing `T ≤ 0` settles at intrinsic and drops.

### B1 — Strategy DSL interpreter + DispersionStrategy

**Build.** `strategy.hpp` grammar; `resolve_strike_by_delta` (bisection on
log-moneyness, monotone per side); `expand_leg` (Single/Straddle/Strangle);
sizing (FixedContracts/TargetVega/Weight) + `CrossLegConstraint`
(FlatVega/VegaNeutralBasket); `LifecycleSpec` (EveryStep/EveryNDays,
HoldToExpiry overlapping cohorts / RollAtHorizon). `DispersionStrategy` re-expresses
P4-1 as a spec + implied-corr signal hook (existing `build_dispersion_book` and
its 5 tests stay green — adapter, not rewrite).

**Gate tests** (`strategy_test.cpp`):
- *Strike-from-delta:* resolved K reprices to `|delta| ≈ target` (1e-4) for
  25Δ put / 40Δ call across tenors; unreachable target ⇒ `InvalidArgument`.
- *Structures:* strangle yields one OTM call + one OTM put at the selected deltas.
- *Flat-vega:* example B ⇒ `Σvega(XOM) + Σvega(SPY) ≈ 0` (priced via PortfolioPricer).
- *Overlapping clips:* EveryStep + HoldToExpiry over K steps ⇒ K cohorts with
  staggered expiries, each aging independently; count decays as they expire.
- *Dispersion parity:* spec-built dispersion book == `build_dispersion_book`
  positions (vega-neutral, implied corr matches closed form) — no regression.

### B2 — Execution + full financing/borrow ledger + hedge overlay

**Build.** `FrictionModel` (modeled spread, per-contract cost, hedge slippage);
`CashLedger` (premium financing at `r`, short-share borrow, shares carry via
`q_eff`); the **hedge overlay routine** — the engine executor that reads a
strategy's `HedgeSpec` (DeltaToZero, cadence daily/at-entry, band) and trades the
shares ledger to satisfy it; wire trade diffing (current vs target book) → fills →
ledger.

**Gate tests:**
- *Zero-friction identity:* `SpreadKind::None` + zero costs ⇒ `BacktestResult`
  bit-identical to B1.
- *NAV reconciliation:* `NAV_d − NAV_{d-1} == Σ axes + shares PnL + financing`
  every step (Taylor tol).
- *Financing:* flat book, no trades ⇒ cash grows at `exp(r·dt)`; short shares
  bleed `borrow_rate·|short|·S·dt`.
- *Hedge overlay:* DeltaToZero daily ⇒ post-hedge `|net delta| ≤ band` each step;
  example A (25Δ put daily-hedged) hedge PnL ≈ −gamma rent (sign check).
- *Friction monotonicity:* drag increases with `half_spread_bps`.

### B3 — TearSheet (incl. vega-scaled) + export + examples

**Build.** `tearsheet` (standard + vega-scaled/per-unit-risk); `write_backtest_tsv`
(deterministic); `examples/dispersion_backtest.cpp` + `examples/strategy_examples.cpp`
(both worked examples) on a synthetic corpus built from the `corpus_test` helpers,
registered under `ATX_BUILD_EXAMPLES`.

**Gate tests:**
- *Tearsheet math:* Sharpe/drawdown/hit-rate/turnover + `return_on_gross_vega`,
  `vega_adj_sharpe`, `pnl_per_vega_traded` vs hand-computed on a known PnL series.
- *Attribution totals close:* tearsheet `Σ attr_* == total_return` (tol).
- *TSV round-trip:* written TSV parses back to the same series (bit-exact doubles).
- *Examples:* both run end-to-end, attribution closes, output deterministic.

---

## Cross-cutting gates (every phase)

- Full atx-vol suite green; `/W4 /permissive- /WX` clean.
- SPY 99.5% in-band untouched (no pricing-path change).
- Backtest output bit-identical across thread counts.
- Attribution closes: `Σ Taylor axes + unexplained + shares + financing == PnL`,
  unexplained small on spot/vol-only steps.
- No paid Databento pull; synthetic corpora for all gates (pilot ingest stays
  operator-gated, P2-6).
- Determinism of the strike-from-delta solver (fixed tol + iteration cap).

## Non-goals / future

- No intraday snapshots this sprint (the `Clock`/timestamp core admits them later
  with no re-arch).
- No listed-expiry snapping in B1 (`TenorSpec::snap_to_listed` is a reserved hook;
  B1 prices on the interpolated surface at `target_T`).
- No variance-swap replication, no American early-exercise *policy* simulation
  (lots settle at intrinsic at expiry; mid-life exercise not modeled).
- No margin/haircut model (cash ledger tracks financing/borrow, not reg margin).
- No new curve kinds required; the analytics-layer extensibility seam (sprint
  `2026-07-05`) remains available if a corpus shape demands it.
