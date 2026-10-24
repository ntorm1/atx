# atx-vol risk engine — first-class P&L explain, theoretical-contract projection, intraday benchmark

**Date:** 2026-10-24
**Status:** Design → plan → build (autonomous run; user directive 2026-10-24).
**Scope:** `atx-vol` only. New module group `risk` (`include/atx/vol/api/risk/`, `src/risk/`), a
SpiderRock intraday market-state loader, a data-free SpiderRock-shaped store generator, a
new benchmark tool, and a rebuild of the historical-VaR pipeline on the new engine.
**Backward compatibility:** NOT required (user ruling). The old `src/analytics/var.*`,
`historical_projection.*` pipeline is replaced; `PortfolioPricer` stays (it is the cold
oracle the new engine is tested against) but is no longer the portfolio engine the
library leads with.

## 0. The ask, verbatim intent

> Build P&L explain (base market environment, shifted market environment, portfolio of shares
> and options → Greek-level breakdown) and the interpolation engine (strike conventions
> log-moneyness ↔ delta ↔ K; vol-slice interpolation at expiries not in the surface; "the same
> 20-delta put, fixed TTE and moneyness, on a historical surface") into a first-class citizen
> at HFT-level throughput for backtest and analytics at scale. Benchmark: SpiderRock intraday
> option data, 10k options on 100 underlyings + 100 share positions, 10:00 → 15:00. Fix
> correctness bugs, maximize performance, no backward compatibility needed while the VaR
> engine pipeline is revamped. Opus 5.5 sub-agent implementers; web research where needed.

## 0.1 Status log

- 2026-10-24 17:10 UTC: user correction — the licensed drop is present as a single parquet in Downloads (2026-10-14, not 2026-08-14). Profiled, partitioned into `C:\atx-data\spiderrock-30min`; §1 fact row rewritten, D7 revised, D16 added; lane C notified (additive `american_only` filter, real-store env-gated test, docs date).
- 2026-10-24 16:40 UTC: builder check (Opus subagent) returned 7 questions + API conflicts;
  resolved as D4 (revised), D5 (revised), D11–D15 below; headers re-patched and re-checked.
- 2026-10-24 15:50 UTC: base lease `pool-34` on `feat/vol-risk-base-20261024` @ main 96cd56db;
  frozen headers `include/atx/vol/api/risk/*.hpp` + `api/marketdata/spiderrock_store.hpp`
  written and compile-checked (`src/risk/risk_api_check.cpp`); `DeltaConvention::Spot` added;
  `risk` test slice + `ATX_VOL_RISK_SOURCES` added. Research brief received (scratchpad).

## 1. Facts that shape the design (measured / found during recon)

| Fact | Source | Consequence |
|---|---|---|
| `PortfolioPricer::pnl_explain` costs ~1.1 ms per unique contract single-thread (2688 uniques: 3.05 s t1, 0.63 s t8; cold Andersen-Lake FD bundle ≈ 17 solves + 2 marks) | `bench/baselines/i7-1260p-clang18-sse2-portfolio.json` | 10k options ≈ 1.4 s per step at 8 threads. Not HFT. The kernel, not the bookkeeping, is the floor |
| σ-family Greek bundle: price + 8 Greeks in one fused quadrature pass, **5.5–7.2 µs per contract** (SPY 23 d, vol-rel-avx2); a family = n_σ (4..16) cold node solves per (expiry, side); calls served through McDonald-Schroder at any spot via the T8 rescale | LEDGER 2026-10-22/23 (L3g, A0); `american.hpp` `AlSigmaFamily`, `family_greeks` | ~150–200× cheaper per contract than the cold bundle. This is the HFT kernel. Refuses typed (near exercise edge, outside σ box, τ < 3 d) → route those lanes cold |
| Serving record tier-0 ladder: 141 ns per contract, but 4.1 s of ladder build for 4 expiries | LEDGER 2026-10-23 (H0-A, H1) | Too expensive to build per 30-min snapshot for 100 names; out of scope for this engine (future kernel) |
| `american_greeks_al` reference Greeks are wrong near the exercise boundary (fixed hS stencil straddles the kink) | LEDGER 2026-10-16 trap | The cold oracle itself is suspect at the edge; tests pin agreement away from the edge and pin typed refusal at it |
| `CurveSurface::w` extrapolates silently (flat vol short of the front pillar, flat total variance past the last); `VolSurface` returns NaN instead | `vol_curve.hpp:385-403` | Every theoretical-contract answer must carry a typed tenor flag; a 7-day contract on a surface whose front pillar is 30 d is an extrapolation, never a silent number |
| Three delta conventions exist and disagree: projection spine = B76 forward only; `strategy.hpp` = American spot; `contract_projection` = American seeded from a carry-discounted spot | `types.hpp:128-160` (AN-P2-6) | One `DeltaConvention` enum with `Forward`, `Spot`, `American`; every solver accepts all three or returns `NotImplemented`, never a different answer under the same name |
| `pnl_attribution` pivots at fixed k = ±0.10; for a 7-day option that is ~6σ√T in the wing → NaN → whole vega P&L falls to `vol_resid` | `pnl_attribution.hpp` | Pivots must be in standardized moneyness (z = k / (σ_atm √T)) |
| `Position` carries only `OptionContract`; shares exist only inside the backtest ledger and the VaR engine (`VarStockPosition`) | `portfolio_pricer.hpp:140-156`, `var.hpp:45` | The new book carries share legs natively |
| Licensed SpiderRock intraday drop IS on this machine: `C:\Users\natha\Downloads\OptionIntradayHist_30min_2026-10-14.parquet` (single DuckDB-written parquet, 1.79 GB, 34,011,291 rows, trading date 2026-10-14, 6067 underliers, 62 columns: `okey_dt` date instead of yr/mn/dy, float32 vendor columns, -99 sentinels in ~1 % of rows, 20 slices 0930–1630 of which 11 fall in 10:00–15:00; 2401 underliers have ≥ 100 usable two-sided contracts at 10:00; 6 % of rows are `exType=European`/`prcFramework=Forward` index products — SPX, NDX, XSP, RUT) | DuckDB profile 2026-10-24 (scratchpad `profile_sr.py`) | Partitioned by `scripts/spiderrock_intraday_to_store.py` (DuckDB, single pass) into the pinned store layout `C:\atx-data\spiderrock-30min\date=2026-10-14\bucket_et=HHMM\` with pinned names/types (float64, int64, utf8 strings, -99 → NULL). The benchmark's licensed run is part of integration 1 (D7 revised); the loader filters to American exercise (D16) |
| SpiderRock `years` is a hybrid vol-time clock (ledger 2026-08-23, VolTime constants); their resolved pricing convention is a discrete-dividend tree, spot = uPrc, rate = row.rate, q = sdiv; srVol is a per-expiry smoothed surface value, not a quote | `bench/oracle/CONVENTIONS.md`, LEDGER 2026-10-16 | Marked vols per contract are exactly the "market environment" an explain engine wants for listed contracts; no refit needed per snapshot |
| Pool: 3 free warm vol slots (pool-34/35/36, `build-vol-rel` + `-avx2` + hygiene warm); host i7-1260P 4P+8E, 15.7 GB RAM → 3 lanes at `-Jobs 4` | `lease-worktree.ps1 -Status`, `atx-vol/CLAUDE.md` | Wave 1 = 3 parallel lanes; wave 2 sequential |
| vol-sprint workflow gates are a closed path registry that fails on unknown paths | `vol-dag` skill | Greenfield module files are unknown paths → this sprint runs on the worktree pool with Opus subagents and fresh reviewers, not through the broker |

## 2. Intent

One engine answers, at the speed of the fastest certified American Greek kernel in the
library, the two questions a desk asks every snapshot:

1. **Explain**: given the book and two market states (base, shifted), where did the P&L come
   from — per position, per underlier, per expiry, per Greek axis, with the vol move split into
   level / skew / curvature, shares included, and the unexplained residual stated honestly.
2. **Project**: given a relative contract ("20-delta put, 7 days") and any market state
   (today's, or a historical surface), what is that contract — strike, log-moneyness, delta
   under a named convention, implied vol at an expiry the surface may not have, mark, Greeks —
   with every interpolation and extrapolation declared.

Both run streaming: a snapshot's Greek frame is computed once and is both the target of the
previous step and the base of the next.

## 3. Goals / non-goals / anti-goals

**Goals**
- G1. `MarketState` + `RiskBook` + `RiskKernel` + `RiskFrame` + `explain` as public Tier-B API
  under `atx/vol/api/risk/` (Tier-A promotion is a later, separate decision).
- G2. Share legs are first-class positions in the book and in every explain output.
- G3. Throughput target (to measure, not assume), host i7-1260P, `vol-rel-avx2`, 8 threads,
  10k options / 100 underliers / ~8 expiries per underlier / 100 share legs:
  frame build (families + per-contract Greeks) ≤ 150 ms per snapshot; Taylor explain + reduce
  between two frames ≤ 5 ms; the whole 10:00 → 15:00 run (11 snapshots, 10 steps) ≤ 2 s
  excluding parquet I/O. Cold oracle for the same book: ≥ 10× slower (that is the baseline
  being replaced, measured in the same report).
- G4. Correctness identities, pinned by tests: eight Taylor terms + unexplained == full
  reprice P&L to ≤ 2 ulp per row (unexplained is the exact remainder; fl(e + fl(t − e)) is
  not always t, so "bit-exact" is not a gate); vega split pieces sum to the vega term to
  ≤ 2 ulp;
  totals bit-identical for any thread count; the family kernel agrees with the cold oracle
  to ≤ 1e-6 relative in price and ≤ 1e-4 relative in delta/vega inside the family box and
  away from the exercise edge; near the edge every lane is routed cold or refused, never
  served from a suspect map.
- G5. Theoretical contracts: strike coordinate conversions (K ↔ log-moneyness ↔ standard
  moneyness ↔ delta under `Forward` / `Spot` / `American`) and `TheoContract` resolution on any
  `PricedSurface`/`PricedSurfaceView`, with a skew-preserving (shape-blend) maturity
  interpolation available beside the linear-total-variance default, and typed
  tenor/moneyness extrapolation flags on every answer. Fixed-(T, k) and fixed-(T, Δ) series
  across a vector of market states in one call.
- G6. Benchmark tool `atx-vol-risk-bench` over the SpiderRock store layout with a
  `--synthetic` generator so it runs and is tested on a bare machine, emitting a JSON report
  + markdown summary with per-stage clocks, throughput, explain-quality statistics and a
  determinism check; a baseline pin under `bench/baselines/`.
- G7. Historical VaR rebuilt on the engine: a reference book of relative option templates +
  shares, replayed across historical states, one-period P&L distribution → VaR / ES.
  `src/analytics/var.*`, `historical_projection.*` deleted; the two dispersion consumers
  ported.

**Non-goals**
- No Python bindings in this sprint (follow-up; the public headers are designed to bind).
- No serving-record (tier-0 ladder) kernel; no GPU.
- No refit of surfaces per snapshot inside the engine — a `MarketState` is built from marks
  and/or an already-fitted `PricedSurface`.
- No sticky-delta re-marking inside explain: both states are real markets; stickiness is a
  scenario concept and stays in `scenario_grid` / `served_analytics`.
- No change to `PortfolioPricer`, `scenario_grid`, `pnl_attribution`, `contract_projection`
  internals beyond what lane D needs to delete; they remain as oracles/tests until a later
  removal decision.

**Anti-goals (failure even if it "works")**
- A number served without its route: every per-lane output carries `RiskRoute`
  (Family / ColdAnalytic / ColdFd / Exercised / Share / Refused) and flags
  (TenorExtrapolated, MoneynessExtrapolated, NearExerciseEdge, MarkMissing, ColdFallback, …).
- NaN that hides a refusal: a refused lane is excluded from totals, counted, and its base
  premium reported; a NaN in a `pnl_*` column is a bug.
- A benchmark that passes only with the licensed data present, or one whose synthetic mode
  exercises a different code path than the licensed mode.
- Thread-count-dependent totals.

## 4. Constraints

- House style: `.agents/cpp/agent.md` (C++20, clang-cl 18, `/W4 /WX`, no UB, Rule of Zero,
  `Result<T>`/`Status` error channel, TDD, 100 columns, no clang-format/tidy runs).
- Build only through `scripts/atx-build.ps1` with `-Preset vol-rel`; focused targets; never
  `atx-vol-tests` in a lane; perf numbers only from `vol-rel-avx2`.
- Pool leases via `scripts/lease-worktree.ps1`; 3 concurrent lanes → `-Jobs 4`.
- Hot paths allocate nothing after `reserve`; SoA columns; deterministic reductions in input
  order (Neumaier compensated sums are allowed and preferred for totals, as in
  `served_analytics`).
- Every new public header is Tier-B (outside the umbrella); the umbrella suite must stay green.
- Time convention: `TimeSpec` (Calendar365 default; VolTime opt-in) is a `MarketState`
  property; a book's option T is derived from `expiry_ns` under the state's `TimeSpec`.

## 5. Architecture

```
                 ┌──────────────────────────────────────────────────────────────┐
  parquet /      │  MarketState (one instant)                                   │
  fitted         │   ts_ns, TimeSpec                                            │
  surfaces  ───▶ │   per uid: UnderlierState{spot, rate curve|flat, q|div sched}│
                 │   per uid: vol source = ContractMarks (listed σ per contract) │
                 │                        and/or SurfaceRef (PricedSurface/View)│
                 └──────────────┬───────────────────────────────────────────────┘
                                │ resolve σ, F, q_eff, T per unique contract
  RiskBook ──▶ PreparedRiskBook ─┤  (uid, K, expiry_ns, side) uniques, grouped (uid, expiry, side);
  (options+shares)               │   share legs separate; weights qty·mult)
                                ▼
                 ┌──────────────────────────────────────────────────────────────┐
                 │  RiskKernel::evaluate(prepared, state, needs) → RiskFrame     │
                 │   Family kernel: per (uid, expiry, side) AlSigmaFamily        │
                 │     (+ carry bumps when rho requested) built once per state;  │
                 │     family_greeks per contract; near-edge / out-of-box → cold │
                 │   Cold kernel: PricedSurface::evaluate_batch or               │
                 │     american_greeks_al / _fd on (S,K,T,σ,r,q)                  │
                 │   RiskFrame: SoA per unique: iv, price, Δ Γ ν θ ρ vanna volga  │
                 │     charm, route, flags; per-uid spot/rate/ts; per-share mark  │
                 └──────────────┬───────────────────────────────────────────────┘
                                ▼
                 ┌──────────────────────────────────────────────────────────────┐
                 │  explain(prepared, base: RiskFrame, shifted: RiskFrame|marks) │
                 │   Taylor (simd pnl_batch): δ γ ν volga vanna θ ρ charm        │
                 │   + shares: qty·dS   + unexplained = reprice − explained       │
                 │   + vega split level/skew/curv/resid (z-pivots per (uid,T))    │
                 │   → ExplainResult: rows (SoA, position order), per-uid /      │
                 │     per-expiry buckets, totals, exclusions (count + premium)  │
                 └──────────────────────────────────────────────────────────────┘
  RiskStream: holds the last RiskFrame; step(state) = evaluate once + explain(prev, cur)

  TheoContract: {uid, maturity spec, StrikeCoord, side} → resolve(state|surface, policy)
    → ResolvedTheo{K, T, expiry_ns, k_log, delta(conv), iv, mark, greeks, flags}
    project_series(spec, states[]) : fixed-(T,k) or fixed-(T,Δ) across history
```

### 5.1 `MarketState` (header `risk/market_state.hpp`)

Value type, non-owning views over caller-owned columns (so a loader can mmap/decode parquet
into SoA once and hand out states without copies); an owning `MarketStateStorage` helper
builds one from scalars for tests and small users.

```cpp
struct UnderlierState {            // one per uid present in the state
  std::uint32_t uid;
  double spot;                     // pricing spot (> 0)
  double flat_rate;                // used when `yield` is empty
  const YieldCurve* yield;         // nullable term curve
  double flat_q;                   // continuous carry yield (sdiv) when no schedule
  std::span<const DividendEvent> cash_divs;  // escrowed into F(T) when non-empty
};

struct ContractMarks {             // listed-contract vol marks for one uid, SoA, sorted
  std::span<const double> strike;            // by (expiry_ns, side, strike); binary search
  std::span<const std::int64_t> expiry_ns;
  std::span<const Side> side;
  std::span<const double> sigma;             // vendor / desk marked vol
  std::span<const double> vendor_price;      // optional (empty) — parity diagnostics only
  std::span<const double> vendor_delta;      // optional
};

class MarketState {
public:
  static Result<MarketState> create(std::int64_t ts_ns, TimeSpec time,
                                    std::span<const UnderlierState> underliers,
                                    std::span<const ContractMarks> marks,   // by uid, may be empty
                                    std::span<const SurfaceRef> surfaces);  // by uid, may be empty
  std::int64_t ts_ns() const noexcept;
  const TimeSpec& time() const noexcept;
  const UnderlierState* underlier(std::uint32_t uid) const noexcept;  // nullptr if absent
  const ContractMarks* marks(std::uint32_t uid) const noexcept;
  SurfaceRef surface(std::uint32_t uid) const noexcept;               // null if absent
  // Resolve one contract: sigma from marks (exact key) else surface.iv(K,T); F, q_eff, r at T.
  Result<ResolvedContract> resolve(std::uint32_t uid, double K, std::int64_t expiry_ns,
                                   Side side) const noexcept;
};

struct ResolvedContract { double T, sigma, forward, q_eff, rate, spot; RiskFlags flags; };
```

Rules: a uid with neither marks nor a surface → `ModelUnavailable` for its contracts and
`MarkMissing`/`SurfaceMissing` flags; a contract whose key is absent from marks falls through
to the surface if present (flag `MarkMissing`), else is refused. Forward: `spot · e^{(r−q)T}`
minus escrowed PV of `cash_divs` before expiry when a schedule is present (the standard escrow;
documented approximation vs. SpiderRock's lattice — see §9).

### 5.2 `RiskBook` (header `risk/risk_book.hpp`)

```cpp
struct OptionLeg { std::uint64_t id; std::uint32_t uid; double K; std::int64_t expiry_ns;
                   Side side; double qty; double multiplier{100.0}; };
struct ShareLeg  { std::uint64_t id; std::uint32_t uid; double qty; };

class RiskBook {           // immutable after create; dedups option uniques on exact bits
public:
  static Result<RiskBook> create(std::span<const OptionLeg> options,
                                 std::span<const ShareLeg> shares);
  std::size_t n_option_legs() const; std::size_t n_unique_options() const;
  std::size_t n_share_legs() const;  std::size_t n_underliers() const;
};
class PreparedRiskBook;    // grouped (uid, expiry_ns, side) ascending-K ladders, aligned SoA,
                           // reverse permutation to leg order; built once; reused per state
```

### 5.3 `RiskKernel` + `RiskFrame` (header `risk/risk_kernel.hpp`, `risk/risk_frame.hpp`)

```cpp
enum class RiskRoute : std::uint8_t { Family, ColdAnalytic, ColdFd, Exercised, Refused };
enum class RiskFlag  : std::uint32_t { TenorExtrapolated=1, MoneynessExtrapolated=2,
  NearExerciseEdge=4, MarkMissing=8, SurfaceMissing=16, OutsideFamilyBox=32, Rolled=64 /*T<=0*/ };

struct RiskNeeds { bool second_order{true}; bool rho{false}; bool charm{true}; };

struct RiskFrame {                 // SoA, one row per unique option, plus per-uid and share rows
  std::int64_t ts_ns;
  std::vector<double> T, sigma, forward, price, delta, gamma, vega, theta, rho, vanna, volga, charm;
  std::vector<RiskRoute> route; std::vector<std::uint32_t> flags; std::vector<Status> status;
  std::vector<double> uid_spot, uid_rate;           // indexed by prepared uid slot
  std::vector<double> share_mark;                   // per share leg = spot
  FrameCounters counters;                           // families built, cold fallbacks, refusals
};

struct FamilyKernelPolicy {
  std::uint16_t n_sigma{12}; double sigma_box_pad{0.25};   // box = [min σ·(1−pad), max σ·(1+pad)]
  double edge_band{0.03};                                   // σ√T distance; inside → cold
  bool carry_bumps_when_rho{true}; unsigned n_threads{0};
  QueryExecution cold_route{QueryExecution::ColdReference};
};
class RiskKernel {                 // strategy; two shipped implementations
public:
  static RiskKernel family(FamilyKernelPolicy);    // HFT kernel
  static RiskKernel cold(bool analytic, unsigned n_threads);   // oracle
  Status evaluate(const PreparedRiskBook&, const MarketState&, RiskNeeds,
                  RiskFrame& out, RiskWorkspace& ws) const;   // allocation-free after reserve
};
```

Family kernel algorithm per state: for each (uid, expiry_ns, side) group: geometry
`prepare_al_geometry(T, r, q_eff)`; σ box from the group's resolved σ range padded; one
`AlSigmaFamily` (anchor = spot for calls, per-strike anchor is NOT needed: the put family's
rescale serves any strike at a few ULP — accepted per the family contract); carry bumps only
when `needs.rho`; then `family_greeks` per contract. Lanes with `status != Ok` or
`near_edge` or `τ < kAlSigmaFamilyMinTau` are collected and run through the cold route in a
second pass (batched, same thread partition rule). Groups are fanned out over the pricing
executor; per-contract writes land in disjoint slots; no reduction happens in the kernel.

### 5.4 `explain` (header `risk/pnl_explain.hpp`)

```cpp
struct ExplainOptions { bool vega_split{true}; double z_ref{1.0}; /* pivots at z∈{−1,0,+1} */
                        bool neumaier_totals{true}; };
struct ExplainRow { id, uid, kind(Option|Share), pv_base, pv_target, pnl_total,
  pnl_delta, pnl_gamma, pnl_vega, pnl_volga, pnl_vanna, pnl_theta, pnl_rho, pnl_charm,
  pnl_unexplained, vol_level, vol_skew, vol_curv, vol_resid, d_spot, d_vol, d_time, d_rate,
  route, flags, status };                         // stored SoA in ExplainResult
struct ExplainTotals { same axes + n_ok, n_excluded, excluded_premium };
struct ExplainResult { SoA columns; totals; by_uid; by_expiry; exclusions by reason; };

Result<ExplainResult> explain(const PreparedRiskBook&, const RiskFrame& base,
                              const RiskFrame& shifted, const ExplainOptions& = {});
Status explain_into(..., ExplainResult& out, RiskWorkspace& ws);   // hot path, no allocation

class RiskStream {   // streaming: one frame per state
public:
  RiskStream(const RiskBook&, RiskKernel, RiskNeeds, ExplainOptions);
  Status start(const MarketState& s0);                      // evaluates frame 0
  Result<const ExplainResult&> step(const MarketState& s1); // evaluates frame 1, explains 0→1, rolls
  const RiskFrame& current() const;
};
```

Semantics (per unique option, base state b, shifted state t):
`dt = (t.ts − b.ts)` under the state `TimeSpec` (years); `dS = S_t − S_b`; `dr = r_t(T_b) − r_b(T_b)`;
`dσ = σ_t(contract) − σ_b(contract)` where σ_t comes from the shifted frame's own resolution
(marks at the same listed contract; or the shifted surface at the SAME absolute strike and at
`T_b` — the common-base-maturity convention the current engine uses, retained). Taylor terms are
the eight analytic terms on the base Greeks (SIMD `pnl_batch`); `pnl_total = w·(P_t − P_b)` with
`P_t` the shifted frame's price for that contract (T_t = T_b − dt); unexplained is the exact
remainder. Shares: `pnl_total = pnl_delta = qty·dS`, every other axis 0. A lane whose base or
shifted row is not `Ok` (rolled past expiry, refused, model unavailable) is excluded from
totals and counted with its base premium `|w·P_b|` — never NaN into a sum.

Vega split per (uid, T_b) group: sample both states' vol at three strikes
`K_z = F_b · exp(z · σ_atm √T_b)`, z ∈ {−z_ref, 0, +z_ref} (σ_atm = base σ at F_b), fit
`dσ(z) ≈ a0 + a1 z + a2 z²`, and split each contract's `pnl_vega` by the fractions
`a0/dσ_i`, `a1 z_i/dσ_i`, `a2 z_i²/dσ_i`, remainder → `vol_resid` (bit-exact partition, as
today). With marks only (no surface), the pivots are the group's own nearest-strike marks
(flag `PivotFromMarks`); a group with < 3 usable marks puts everything in `vol_resid`.

### 5.5 Projection (headers `risk/strike_coord.hpp`, `risk/theo_contract.hpp`)

```cpp
enum class DeltaConvention : std::uint8_t { Forward = 0, American = 1, Spot = 2 }; // types.hpp, extended in place
enum class StrikeCoordKind : std::uint8_t { Strike, LogMoneyness, StandardMoneyness, Delta };
struct StrikeCoord { StrikeCoordKind kind; double value; DeltaConvention delta_conv{Forward}; };

enum class TenorInterp : std::uint8_t { TotalVariance = 0, ShapeBlend = 1 };
enum class TenorExtrapPolicy : std::uint8_t { Flag /*serve + flag, default*/, Forbid /*OutOfRange*/ };  // `ExtrapPolicy` is taken by correction.hpp

struct CoordResult { double K, k_log, standard_moneyness, delta, T, forward; std::uint32_t flags; };
Result<CoordResult> convert_strike_coord(SurfaceRef, double T, Side, StrikeCoord from,
                                         StrikeCoordKind to, const ProjectionPolicy&);
Status convert_strike_coord_batch(SurfaceRef, span<const double> T, span<const Side>,
                                  span<const StrikeCoord>, StrikeCoordKind to,
                                  span<CoordResult> out, const ProjectionPolicy&);

struct TheoContract { std::uint32_t uid; MaturitySpec maturity /*years|days|months|absolute*/;
                      StrikeCoord strike; Side side; double multiplier{100.0}; };
struct ResolvedTheo { OptionLeg leg; double k_log, delta, iv, mark; AmericanGreeks greeks;
                      double forward; std::uint32_t flags; TheoStatus status; };
Result<ResolvedTheo> resolve_theo(const TheoContract&, SurfaceRef, std::int64_t valuation_ns,
                                  const ProjectionPolicy&);   // Greeks: cold analytic; HFT Greeks = put the leg in a RiskBook
Status project_series(const TheoContract&, span<const SurfaceRef>, span<const std::int64_t> ts,
                      const ProjectionPolicy&, span<ResolvedTheo> out);  // fixed-(T,k) / fixed-(T,Δ)
```

`ProjectionPolicy { TenorInterp interp{TotalVariance}; TenorExtrapPolicy extrap{Flag};
DeltaConvention delta{Forward}; double delta_tol{1e-7}; TimeSpec time; }`.
`ShapeBlend` is the SpiderRock-style vol-multiple blend already specified in
`projection.hpp` (`InterpMode::ShapeBlend`) ported to `CurveSurface` as a query-side
mode: `PricedSurface::iv(K, T, TenorInterp)` overload; the default overload is unchanged
(bit-identical). Delta solves: `Forward` = closed form B76 inversion; `Spot` = Black spot
delta (`e^{−qT} N(d1)`), closed form; `American` = the existing cold-confirmed secant on
`PricedSurface::delta` (`solve_american_delta` COPIED from `contract_projection.cpp` into
`src/risk/american_delta_solve.*`; `contract_projection` itself is untouched this sprint). Every answer carries `TenorExtrapolated` when `extrapolates_tenor(T)` and
`MoneynessExtrapolated` when the slice's `is_extrapolated(k)`.

The user's walk-through becomes:
```cpp
TheoContract p20{uid, MaturitySpec::absolute(exp_2026_10_30), StrikeCoord::delta(-0.20, Forward), Side::Put};
ResolvedTheo today = resolve_theo(p20, today_surf, ts_today, pol, &kernel).value();
TheoContract same{uid, MaturitySpec::years_of(today.T), StrikeCoord::log_moneyness(today.k_log), Side::Put};
project_series(same, hist_surfaces, hist_ts, pol, out);   // same TTE + k on every historical state
```

### 5.6 Historical VaR (header `risk/historical_var.hpp`) — lane D

`HistoricalVarBook { vector<TheoContract> options (with qty); vector<ShareLeg> shares; }`.
For each historical state pair (b_i, t_i): resolve the templates on b_i (fixed Δ or fixed k),
build a `RiskBook` for that scenario, evaluate b_i and t_i with the chosen kernel, `explain`,
collect `pnl_total` (and the Greek split, so VaR can be attributed by axis). Output:
per-scenario frames, nearest-rank VaR/ES at the requested confidences, per-axis contribution
of the tail scenarios. `dispersion_book_var` (two call sites) is re-pointed at this.
`src/analytics/var.cpp`, `var.hpp`, `var_report.*`, `var_validation.*`,
`historical_projection.*`, `bench/var_bench.cpp`, `tests/var_test.cpp`,
`tests/historical_projection_test.cpp`, `bench/run_historical_projection_gate.ps1` are deleted
or rewritten to the new API (lane D decides per file; nothing is left half-dead).

### 5.7 SpiderRock market-state loader + synthetic store — lane C

`include/atx/vol/api/marketdata/spiderrock_store.hpp`, `src/marketdata/spiderrock_store.cpp`:
- `SpiderRockStoreSpec { store_root, date, buckets_et[], underliers[] (empty = top-N by rows),
  max_underliers, TimeSpec }`.
- `load_spiderrock_bucket(spec, bucket) → SpiderRockBucket` : SoA columns per row (uid mapped
  from `undSecKey_tk` via a stable `UnderlierTable`), `okey_xx` strike, expiry instant from
  `okey_yr/mn/dy` (16:00 ET settlement via `vol_time.hpp` helpers; the `years` column is kept
  as `vendor_T` for diagnostics), side, `srVol`, `srPrc`, `de/ga/ve/th/rh`, `uPrc`, `rate`,
  `sdiv`, `ddiv`, `bidPrc/askPrc`. Partition-pruned Arrow read (reuse the column/null rules of
  `tools/oracle_cohort_reader.cpp`; both utf8/large_utf8).
- `market_state_from_bucket(bucket) → MarketStateStorage` (per-uid spot = uPrc median of the
  uid's rows at that bucket; rate = row rate (flat per uid); q = sdiv; cash dividends: the
  bucket's `ddiv` is a PV sum, not a schedule → escrow it as a single event at the contract's
  expiry… see §9 decision D4) + `ContractMarks` from `srVol`.
- `SyntheticSpiderRockStore::write(spec, out_dir)` — SpiderRock-shaped parquet partitions
  (`date=<d>/bucket_et=<HHMM>/data.parquet`, the reader's column subset + `atmVol`,
  large_utf8 strings) with a deterministic eSSVI-driven market that MOVES across buckets
  (spot random walk, level/skew shocks, time decay) so the explain has something to explain.
  Shared by the bench (`--synthetic`) and the tests. Lives in `src/marketdata/` behind the
  public header (not test-only: the bench ships it).

### 5.8 Benchmark tool `atx-vol-risk-bench` — lane C (+ integration)

`tools/risk_bench_main.cpp` (+ `tools/risk_bench.hpp/.cpp` for the testable core):
```
atx-vol-risk-bench --store <root> --date 2026-10-14 --from 1000 --to 1500
                   [--synthetic <dir>] [--underliers 100] [--options 10000] [--shares 100]
                   [--kernel family|cold|both] [--threads 8] [--needs rho]
                   [--out report.json] [--md summary.md] [--seed 1]
```
Book construction (deterministic from `--seed`): top-100 underliers by row count at the first
bucket; per underlier 100 contracts chosen by liquidity (two-sided, |Δ| in [0.05, 0.95], across
≥ 4 expiries, both sides); qty ∈ {−10..+10} nonzero; 100 share legs = one per underlier, qty =
−round(book net delta per uid / 100)·100 (a delta-hedged book, so the explain is interesting).
Run: `RiskStream` over buckets `--from..--to`; per step record clocks (bucket load, state
build, families built, frame evaluate, explain) and counts (routes, flags, exclusions); after
the run: contracts/s and legs/s per stage, p50/p95 over steps, explained ratio
Σ|unexplained| / Σ|pnl_total|, per-axis shares, FRTB-style Spearman ρ and KS between explained
and actual per-position P&L (see research brief), vendor parity (our Δ/ν vs `de`/`ve`, MAE,
symmetric rel) when vendor columns are present, thread determinism (`--threads 1` vs N totals
bit-equal), and the cold-kernel comparison when `--kernel both`. JSON schema versioned; a
`bench/baselines/i7-1260p-clang18-avx2-risk-bench-synthetic.json` pin and a
`…-risk-bench-spiderrock-2026-10-14.json` pin (licensed store, 11 slices 1000–1500) are
produced in the integration stage from `vol-rel-avx2`.

## 6. Correctness fixes folded in

| # | Defect | Fix |
|---|---|---|
| F1 | Shares absent from explain | `ShareLeg` first-class; `pnl_delta = qty·dS` |
| F2 | Silent tenor extrapolation in `CurveSurface::w` on the portfolio path | `RiskFlag::TenorExtrapolated` on every resolved lane; `ExtrapPolicy::Forbid` refuses |
| F3 | Three delta conventions | `DeltaConvention{Forward, American, Spot}` honoured by every solver; `NotImplemented` only where a convention is impossible, never a silently different answer |
| F4 | `pnl_attribution` fixed k = ±0.10 pivots | standardized-moneyness pivots `z∈{−1,0,1}` |
| F5 | `american_greeks_al` suspect near the exercise edge | family kernel refuses near-edge → cold; cold lanes near the edge carry `NearExerciseEdge`; tests pin exclusion, not a number |
| F6 | `pnl_explain` rolled-past-expiry lanes become `InvalidContract` and vanish from totals silently | explicit `Rolled` exclusion with premium and count |
| F7 | Vendor T (`years`) vs our clock mismatch silently shifts theta | the state's `TimeSpec` decides; vendor T retained as diagnostic; bench reports the two side by side |
| F8 | `contract_projection` resolves delta with American delta only; projection spine Forward only | one `convert_strike_coord` with the three conventions |

## 7. Lanes, ownership, sequencing

All lanes implement against the **frozen public headers committed on the sprint base
branch by the orchestrator before dispatch** (the API contract above, written out in full).
Lanes may add private headers under `src/risk/` freely; a lane needing a change to a frozen
public header requests it in its report (the orchestrator applies it on the base and rebases).

| Lane | Owns (create/modify) | Forbidden | Depends on |
|---|---|---|---|
| **A — core engine** | `src/risk/{market_state,risk_book,prepared_risk_book,risk_kernel_family,risk_kernel_cold,risk_frame,pnl_explain,risk_stream}.cpp`, `src/risk/*.hpp`, `tests/risk_{market_state,risk_book,risk_kernel,pnl_explain,risk_stream}_test.cpp`, its lines in `CMakeLists.txt` (`ATX_VOL_RISK_SOURCES`) and `tests/slices.cmake` (`risk` slice) | everything under `src/analytics/`, `src/backtest/`, `vol_curve.*`, `priced_surface.*`, `types.hpp` | frozen headers |
| **B — projection** | `src/risk/{strike_coord,theo_contract}.cpp`, `types.hpp` (extend `DeltaConvention`), `vol_curve.hpp/.cpp` + `priced_surface.hpp/.cpp` + `priced_surface_view.hpp/.cpp` (ShapeBlend overloads only, additive), `tests/risk_{strike_coord,theo_contract}_test.cpp`, `tests/priced_surface_shape_blend_test.cpp` | `src/risk/` files of lane A, `src/analytics/`, marketdata | frozen headers; may link lane A's `RiskKernel` only through the nullable pointer (cold path when null) so it compiles and tests without A |
| **C — data + bench** | `include/atx/vol/api/marketdata/spiderrock_store.hpp`, `src/marketdata/spiderrock_store.cpp`, `src/marketdata/synthetic_spiderrock_store.cpp`, `tools/risk_bench{.hpp,.cpp,_main.cpp}`, `tests/spiderrock_store_test.cpp`, `tests/risk_bench_test.cpp`, its `CMakeLists.txt` lines (tool target, `ATX_VOL_MARKETDATA_SOURCES`) | `src/risk/` | frozen headers; the bench TU compiles (`check`) against them; linking the tool and running the end-to-end bench test is an integration-stage gate |
| **D — VaR rebuild + dispersion port** (wave 2) | `src/risk/historical_var.cpp`, `tests/risk_historical_var_test.cpp`, deletion/rewrite of `src/analytics/var*`, `historical_projection*`, `bench/var_bench.cpp`, `bench/contract_projection_bench.cpp` (the var half), `bench/run_historical_projection_gate.ps1`, `tests/var_test.cpp`, `tests/var_report_test.cpp`, `tests/var_validation_test.cpp`, `tests/historical_projection_test.cpp`, `src/backtest/dispersion_run.cpp` + `listed_dispersion_pipeline.cpp` call sites and their tests (`tests/dispersion_run_test.cpp`, `tests/listed_dispersion_pipeline_test.cpp`), `research/include/atx/vol/research/{dispersion_backtest,dispersion_run,listed_dispersion_pipeline}.hpp`, `tools/spy_dispersion_backtest.cpp` | lanes A/B/C files except through their public headers | A + B merged |

Wave 1: A, B, C in parallel on pool-34/35/36 (`-Jobs 4`). Each lane: build focused test target
→ anchored ctest → report → fresh Opus reviewer (exact SHA) → one fix round → re-review.
Integration 1: orchestrator merges A+B+C into the sprint branch, links the bench tool, runs the
synthetic end-to-end, pins the baseline, runs `vol-hygiene` on changed headers and the umbrella
suite. Wave 2: lane D on one slot. Integration 2: full `atx_vol_fast` label on `vol-rel`
(release qualification), CHANGELOG, LEDGER, sprint note, README section.

## 8. Gates (falsifiable)

- Each lane's focused target builds warning-clean under `vol-rel`; anchored suites pass with
  positive executed counts; `check` on every changed TU; `vol-hygiene` on changed headers.
- A: `RiskExplain.TaylorIdentityBitExact`, `RiskExplain.VegaSplitPartitionBitExact`,
  `RiskExplain.SharesAreDeltaOnly`, `RiskExplain.ThreadCountBitIdentical{1,2,8}`,
  `RiskKernel.FamilyMatchesColdInsideBox` (≤1e-6 price, ≤1e-4 Δ/ν rel on a 60-contract
  synthetic eSSVI book), `RiskKernel.NearEdgeRoutesCold`, `RiskStream.FramesComputedOncePerState`
  (counter: 11 states → 11 evaluations, 10 explains), `RiskExplain.ExcludedLanesCarryPremium`.
- B: `StrikeCoord.RoundTripsAllPairs`, `StrikeCoord.ThreeDeltaConventionsDifferOnHighCarry`,
  `TheoContract.FixedMoneynessSeriesHoldsTAndK`, `TheoContract.TenorExtrapolationIsFlagged`,
  `PricedSurface.ShapeBlendDefaultOverloadBitIdentical`, `PricedSurface.ShapeBlendPreservesSkew`.
- C: `SpiderRockStore.ReadsBothStringEncodings`, `SpiderRockStore.MarketStateFromBucketKeys`,
  `SyntheticStore.ElevenBucketsMoveTheMarket`, `RiskBench.SyntheticEndToEndReportSchema`
  (integration), `RiskBench.ThreadDeterminism` (integration).
- D: `HistoricalVar.FixedDeltaAndFixedMoneynessTemplates`, `HistoricalVar.SharesInScenarioPnl`,
  dispersion suites still green.
- Release: `-Ctest -Preset vol-rel -L atx_vol_fast` green; `bench/baselines/…risk-bench…json`
  committed with the numbers table in the sprint note; G3 targets reported as measured
  (pass or miss, stated).

## 9. Decisions made here (the builder does not re-open these)

- D1. Namespace stays flat `atx::vol`; new headers under `api/risk/`; Tier-B.
- D2. Book keys options by absolute `expiry_ns`; T is derived per state. `retime` is gone.
- D3. The family kernel is the default `RiskStream` kernel; the cold kernel is the oracle and
  the fallback route. Serving records are not a kernel in this sprint.
- D4 (revised after the builder check). SpiderRock publishes one `(rate, sdiv, ddiv)` tuple
  per EXPIRY, with `ddiv` the PV of cash dividends up to that expiry. `UnderlierState` therefore
  carries an optional `term_carry` span of `ExpiryCarry{expiry_ns, pv_divs, rate, q}` rows
  (exact-expiry lookup) and the loader fills one row per (uid, expiry) from the medians over
  that expiry's rows, so `F = (S − pv_divs)·e^{(r−q)T}` reproduces the vendor's forward basis
  per expiry with no double counting. The schedule path (`cash_divs`, amounts at the ex-date,
  discounted at the rate) remains for states built from a real dividend calendar; `term_carry`
  wins when a row matches. Vendor price columns tell the user the remaining price-level gap
  to SpiderRock's lattice.
- D5 (revised). `ExplainOptions::vol_move` selects the vol-move convention: `ContractOwn`
  (default: σ_t is the shifted frame's own σ at T_t — a mark, or `surface.iv(K, T_t)`; roll-down
  is part of the vol move; SpiderRock's fixed-strike `iVolMove`; the only definition a
  marks-only state admits) or `CommonBaseMaturity` (σ_t = `shifted_state.sigma_at(contract,
  T_b)`, the `PortfolioPricer::pnl_explain` convention used by the oracle-parity test). The
  shifted mark is always at the rolled T_t. Veta is not a term; it lands in unexplained.
- D6. Totals are Neumaier-compensated sums in leg input order on the calling thread;
  per-lane work is fanned over the process pricing executor with disjoint slots. Determinism
  across thread counts is a gate, not a goal.
- D7 (revised 17:10 UTC). The licensed store exists (`C:\atx-data\spiderrock-30min`, date
  2026-10-14, partitioned from the Downloads parquet by `scripts/spiderrock_intraday_to_store.py`).
  Lane C still builds and tests against the synthetic store (identical loader/engine path);
  the licensed run (10:00–15:00, 11 slices, 100 uids / 10k options / 100 shares) is part of
  integration 1 and produces its own baseline pin. Lane C adds one env-gated real-store
  test (`ATX_VOL_SPIDERROCK_STORE`) that is skipped when the store is absent.
- D8. No Python bindings; no serving-record kernel; no sticky-delta scenarios in explain.
- D9. The research brief (`scratchpad/research-pnl-explain.md`, cited sources S1–S41) informs:
  the bench's explain-quality statistics use the FRTB PLA cutoffs verbatim (green: Spearman
  > 0.80 and KS < 0.09; red: Spearman < 0.70 or KS > 0.12; KS on the ECDF of the per-step
  series) as a diagnostic, not a regulatory number; `ExplainOptions::trapezoid_second_order`
  (SpiderRock's risk-cube convention, free in a stream) is shipped as an opt-in; a resolved
  theoretical contract always carries the Black-76 forward delta beside the policy's delta.
  Sequential-revaluation / Shapley attribution, share-leg dividend and financing lines, and
  a calendar-arbitrage-safe (monotone-ATM) tenor interpolation are recorded as follow-ups,
  NOT built in this sprint (Taylor + exact remainder only).
- D10. Names fixed by the compile check on the base: `TenorExtrapPolicy` (`ExtrapPolicy` is
  correction.hpp's), `CarryFrom` (`CarrySource` is deamer.hpp's). `resolve_theo` takes no
  kernel pointer (D5 of §5.5 as written here supersedes the earlier sketch).
- D11 (builder check Q2). Near the exercise edge the family refuses gamma/theta/charm inside
  its own fixed band (`kAlFamilyGreekEdgeBand` = 0.25 σ√T, not configurable); the kernel sends
  the WHOLE lane cold (`FamilyKernelPolicy::cold_near_edge`, default on), flags
  `NearExerciseEdge | ColdFallback`, and the lane stays in the totals. The bench reports the
  cold-lane fraction so the 150 ms target is judged with it.
- D12 (Q3). Spot is always the `UnderlierState`'s; `CarryFrom` decides forward/carry; AL
  options precedence policy → surface `pricing().al_opts` → `al_fast_opts()`; the cold kernel
  prices resolved inputs through `american_greeks_al`/`_fd` (never `evaluate_batch`), so on a
  `market_state_from_surfaces` state it equals `PricedSurface::greeks_analytic` bit for bit.
- D13 (Q5). Historical VaR = one-period P&L per (base_i → shifted_i) pair on surface-backed
  states (`market_state_from_surfaces`); templates re-resolved on each base. `dispersion_book_var`
  is re-pointed at it with consecutive historical surface sets as pairs; its tests are
  RE-PINNED (BREAKING, recorded in CHANGELOG) — the user ruled out backward compatibility for
  the VaR pipeline. Marks-only states cannot host templates: `historical_var` refuses them.
- D14 (Q6/Q7). `StrikeCoordKind::StandardMoneyness` keeps the library's meaning (K/S);
  `StandardizedMoneyness` (z = k/(σ_atm√T)) is added as its own kind. `MoneynessExtrapolated`
  is best effort: lane B adds `moneyness_extrapolated(K, T)` to `PricedSurface`/`View`
  (bracketing slices' `is_extrapolated(k)`; parametric curves never flag; a view answers from
  its record or false).
- D15. "Allocation-free" is bounded, not zero, for the family kernel (families are
  reconstructed per state; `FrameCounters` reports the counts); the lane test pins the bound.
- D16. The SpiderRock loader filters to American exercise (`SpiderRockStoreSpec::american_only`,
  default true, additive): rows with `exType != "American"` (SPX/NDX/XSP/RUT — European,
  forward-framework index products, 6 % of rows) are dropped and counted before top-N
  underlier selection; a store without the column (synthetic, old oracle drop) is treated as
  all-American. Reason: the family kernel is American-on-spot and vendor parity on European
  rows would fail by construction. A European route is deferred, not faked. The ingest also
  normalizes the drop: float32 → float64, -99 → NULL on every vendor column, `okey_dt` →
  `okey_yr/mn/dy`, `bucket_et` = America/New_York HHMM of the slice instant; strings are utf8
  (not large_utf8) and every reader accepts both.

## 10. Left to the builder

- Exact σ-box padding and node count per family (start at `n_sigma=12`, pad 25%; tune on the
  synthetic book; record the measured accuracy/throughput trade-off in the sprint note).
- Internal SoA layouts, alignment, chunk sizes, the exact fan-out shape over the executor.
- The synthetic market's stochastic model parameters (must be deterministic by seed and must
  produce nonzero moves on every axis).
- JSON report field names (document them in `docs/risk-bench.md`).
- How much of `tests/portfolio_pricer_test.cpp` to borrow as oracle fixtures.
