# atx-vol Theo Module (theo.hpp) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a theoretical-value ("theo") engine for atx-vol: a breakeven-vol label factory (delta-hedged replay + root-find over the American pricer), a realized-vol estimator suite, and an overlay-based `TheoEngine` that produces per-contract fair vol / fair price / edge beside (never instead of) the served market surface.

**Architecture:** Three new Tier-B modules — `realized_vol.hpp` (OHLC RV estimators, greenfield), `breakeven.hpp` (σ-parameterized single-option replay + safeguarded root-find), and `theo.hpp` (vocabulary + `TheoEngine` composing `ITheoOverlay` adjustments over a served `PricedSurface`, with an `IFairVolModel` seam for ML). **The replay is an extension of the existing backtest engine, not a parallel engine:** `bev_replay_pnl` is declared in `breakeven.hpp` but implemented inside `src/backtest.cpp`, where it directly reuses the engine's file-local accounting primitives (`HedgeLedger` :602, financing semantics :3686-3767, expiry-at-intrinsic settle :3795-3797) in the same TU. Integration into the backtest run is read-only via `IStrategy::signals()` columns; the `run_backtest` step loop and the mark bit-identity chains are untouched.

**Tech Stack:** C++20, atx-core `Result<T>`/`Status`, Andersen-Lake American engine (`american.hpp`), `PricedSurface`/`SurfaceDb`, GoogleTest, Google Benchmark, `detail/parallel_for.hpp`.

**Research basis:** `docs/research/2026-08-08-theo-fair-vol-breakeven-deep-dive.md` (theo-stack layering, breakeven-vol math incl. Ahmad-Wilmott hedge-vol choice, Dupire fair skew, Hull-Li-Qiao supervised BEV, Derman-Kamal discrete-hedge noise, event-variance decomposition, VRP-vs-alpha caveat).

## Global Constraints

- C++20, clang-cl 18, `/W4 /permissive- /WX`; build only via `scripts\atx-build.ps1` (dev preset iterate; `hygiene` preset is the include gate; full suite only at gate time).
- 100-column limit (`.clang-format` ColumnLimit 100). Never reformat untouched neighbors (`ReflowComments: false`; formatting collateral gets reverted).
- No exceptions in library API: `Result<T>`/`Status`, `Ok(...)`/`Err(ErrorCode::X, "Component: lowercase description")`. Factory pattern `static Result<T> create(...)` for classes with invariants.
- Naming: types `PascalCase`, functions/methods `snake_case`, private members trailing `_`, constants `kCamelCase`, `enum class : std::uint8_t` with explicit values. `#pragma once`. `[[nodiscard]]` on every Result-returning/query entry, `noexcept` on leaf math.
- Config structs: all fields defaulted, DESIGNATED INITIALIZERS ONLY comment + arity pin via `detail/aggregate_arity.hpp` (mirror `AlOpts` at `american.hpp:67-94`).
- New modules are **Tier-B**: `include/atx/vol/*.hpp`, NOT added to the `vol.hpp` umbrella (Tier-A manifest is pinned to 58 in `tests/vol_umbrella_test.cpp:322`).
- Do NOT modify `RunConfig` (arity static_assert 18 on main + hand-kept pybind list) and do NOT edit the `run_backtest` step loop; live-backtest integration is `IStrategy::signals()`/`StepObserver` only. The BEV replay entry point is ADDED to `src/backtest.cpp` as a new engine capability (user directive: build out the existing engine, no parallel replay engine) — it reuses the file-local `HedgeLedger` and mirrors the engine's financing/settlement code, but never touches the step loop itself; the full backtest suite must stay green and bit-identical after the addition.
- The `atx-vol` library target must not gain includes from `tools/` or `research/` roots (documented layering leak — don't deepen it).
- Bit-determinism: any parallel path uses `detail/parallel_for.hpp` (contiguous ranges, disjoint writes) and must be bit-identical for any `n_threads`; bounded loops with `kCamelCase` iteration caps (JPL Rule 2 comment convention, cf. `types.hpp:161-163`).
- Hot paths: no dynamic allocation after init; caller-owned-output `*_into` variants with span-size validation before any mutation; SoA result frames.
- Tests: GoogleTest, `TEST(Suite, Condition_Expectation)`, appended to `atx-vol/tests/CMakeLists.txt` source list with a `# THEO-<n> — appended target:` comment; slow suites added to `ATX_VOL_SLOW_FILTER`.
- No TODO/FIXME comments in code — residual work is recorded in the sprint closeout doc (Task 11).
- Commits: conventional style (`feat(vol): ...`), one commit per green step-cycle.
- Day-count conventions: option T in years ACT/365.25 (analytics convention); RV annualization 252 (matches `RealizedVarianceSpec` default).

## Design rationale (from the code review)

- **Why the replay is an engine extension, not a `run_backtest` call:** the step loop's hedge delta is always the fitted-surface market delta (`src/backtest.cpp:3310-3317`); a BEV label needs hedging at the *trial* vol. Driving `run_backtest` per Brent iteration costs ~1-2 s per 250-day replay; the σ-parameterized replay entry costs ~17 ms (1 analytic solve/day at `al_fast`, price+delta+gamma from the base boundary with empty `GreekNeeds`) → ~0.2-0.3 s per label, ~60-80 core-hours per 1M labels. Per the user directive it is implemented INSIDE `src/backtest.cpp` (declared in `breakeven.hpp`) so the engine gains the capability and the accounting primitives are reused in-TU rather than copied.
- **Accounting semantics are reused, not reinvented:** the replay uses the engine's own `HedgeLedger` (:602 — get/add share ledger; band/slippage/cash-settle ordering exactly as `hedge_daily` :694-714) and mirrors the adjacent financing (:3686-3767) and expiry-at-intrinsic (:3795-3797) blocks, with financing ON by default (backtest defaults are OFF for the B1 identity; a label must be carry-faithful).
- **Early exercise:** main's engine simulates NO early exercise (WS-F F3 comment, `src/backtest.cpp:523-553` — expiry settlement is the only exercise event). The replay path closes that gap for single-option replays with a file-local pure rule matching B3's `should_exercise_early(intrinsic, extension_value, threshold)` semantics (`feat/backtest-lakehouse` backtest.hpp:636); documented beside the WS-F F3 banner as a replay-path-only extension, unification recorded in the sprint doc.
- **Theo is an overlay beside the mark, never a mark substitute:** archive round-trip zero-theo-drift, `greeks().price ≡ fair_value()`, settlement-memo, and NAV-reconcile are pinned bit-identity contracts; `theo.hpp` therefore produces a separate value type and never mutates or replaces a surface.
- **`compute_surface_analytics` is the shape template** (`analytics.hpp:441-449`): pure aggregator over a served `PricedSurface` with a defaulted config — `TheoEngine`/`compute_theo_sheet` follows it.
- **RV estimators are greenfield** (only a close-to-close accumulator exists, `derivatives.hpp:244-`); the theo forecast overlay and the ML feature set both need them, so they ship first as a standalone module.

## File Structure

- Create: `atx-vol/include/atx/vol/realized_vol.hpp` + `atx-vol/src/realized_vol.cpp` — OHLC bar type + estimator suite.
- Create: `atx-vol/include/atx/vol/breakeven.hpp` — BEV public API (day-state/spec/config types, `bev_replay_pnl`, root-find, batch, loader declarations).
- Modify: `atx-vol/src/backtest.cpp` — `bev_replay_pnl` implementation (engine extension; reuses file-local `HedgeLedger` + accounting semantics; step loop untouched).
- Create: `atx-vol/src/breakeven.cpp` — root-find, batch runner, SoA label frame, path loader (layered on the engine's replay).
- Create: `atx-vol/include/atx/vol/theo.hpp` + `atx-vol/src/theo.cpp` — theo vocabulary, overlay interface, engine, sheet aggregator, model seam.
- Create: `atx-vol/examples/bev_label_factory.cpp` — label-factory driver over a `SurfaceDb`.
- Create: `atx-vol/bench/theo_bench.cpp` — replay + engine benchmarks (target `atx-vol-theo-bench`).
- Create: `atx-vol/tests/realized_vol_test.cpp`, `atx-vol/tests/breakeven_test.cpp`, `atx-vol/tests/theo_test.cpp`, `atx-vol/tests/bev_label_factory_gate_test.cpp`.
- Modify: `atx-vol/CMakeLists.txt` (3 sources appended :18-135; example block; bench target), `atx-vol/tests/CMakeLists.txt` (4 test files appended).
- Create: `atx-vol/sprints/2026-08-XX-theo-module-sprint-summary.md` (closeout, Task 11).

---

### Task 1: Realized-vol estimator suite (`realized_vol.hpp`)

**Files:**
- Create: `atx-vol/include/atx/vol/realized_vol.hpp`
- Create: `atx-vol/src/realized_vol.cpp`
- Test: `atx-vol/tests/realized_vol_test.cpp`
- Modify: `atx-vol/CMakeLists.txt` (append `src/realized_vol.cpp`), `atx-vol/tests/CMakeLists.txt` (append test)

**Interfaces:**
- Consumes: `atx::vol` types re-exports (`types.hpp`: `Result`, `Status`, `Err`, `Ok`, `ErrorCode`).
- Produces (all in `namespace atx::vol`):
  - `struct OhlcBar { std::int64_t ts_ns{0}; double open{0}, high{0}, low{0}, close{0}; };`
  - `enum class RvEstimator : std::uint8_t { CloseToClose = 0, Parkinson = 1, GarmanKlass = 2, RogersSatchell = 3, YangZhang = 4 };`
  - `[[nodiscard]] Result<double> realized_vol(std::span<const OhlcBar> bars, RvEstimator est, double annualization = 252.0);` — annualized vol (not variance) over the whole span.
  - `struct RvPanel { std::array<double, 4> vol{}; std::array<std::uint16_t, 4> window{5, 21, 63, 252}; };`
  - `[[nodiscard]] Result<RvPanel> realized_vol_panel(std::span<const OhlcBar> bars, RvEstimator est = RvEstimator::YangZhang, double annualization = 252.0);` — trailing windows ending at the last bar; window falls back to available length if shorter (flagged by `vol = NaN` when < 2 bars).

- [ ] **Step 1: Write the failing tests**

```cpp
// atx-vol/tests/realized_vol_test.cpp
#include "atx/vol/realized_vol.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>

namespace atx::vol {
namespace {

// Deterministic GBM daily bars: drift-free, sigma annualized, 252 steps/yr.
std::vector<OhlcBar> synth_gbm_bars(double sigma, std::size_t n, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> z(0.0, 1.0);
  const double dt = 1.0 / 252.0, sq = sigma * std::sqrt(dt);
  std::vector<OhlcBar> bars;
  bars.reserve(n);
  double s = 100.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double open = s;
    const double close = open * std::exp(-0.5 * sq * sq + sq * z(rng));
    // Intraday extremes: bracket open/close with a half-range excursion.
    const double ex = 0.5 * sq * std::abs(z(rng));
    const double hi = std::max(open, close) * std::exp(ex);
    const double lo = std::min(open, close) * std::exp(-ex);
    bars.push_back(OhlcBar{static_cast<std::int64_t>(i) * 86'400'000'000'000LL,
                           open, hi, lo, close});
    s = close;
  }
  return bars;
}

TEST(RealizedVol, CloseToCloseRecoversGbmSigmaWithinSamplingError) {
  const auto bars = synth_gbm_bars(0.20, 5000, 42u);
  const auto rv = realized_vol(bars, RvEstimator::CloseToClose);
  ASSERT_TRUE(rv.ok());
  EXPECT_NEAR(*rv, 0.20, 0.01);  // ~2*sigma/sqrt(2n) sampling band
}

TEST(RealizedVol, AllEstimatorsAgreeOnGapFreeGbmWithinTolerance) {
  const auto bars = synth_gbm_bars(0.30, 5000, 7u);
  for (auto est : {RvEstimator::Parkinson, RvEstimator::GarmanKlass,
                   RvEstimator::RogersSatchell, RvEstimator::YangZhang}) {
    const auto rv = realized_vol(bars, est);
    ASSERT_TRUE(rv.ok());
    EXPECT_NEAR(*rv, 0.30, 0.05) << static_cast<int>(est);
  }
}

TEST(RealizedVol, TwoBarMinimumEnforced) {
  const auto bars = synth_gbm_bars(0.20, 1, 1u);
  EXPECT_FALSE(realized_vol(bars, RvEstimator::CloseToClose).ok());
}

TEST(RealizedVol, NonPositiveOhlcRejected) {
  std::vector<OhlcBar> bars = synth_gbm_bars(0.20, 10, 3u);
  bars[4].low = 0.0;
  EXPECT_FALSE(realized_vol(bars, RvEstimator::YangZhang).ok());
}

TEST(RealizedVol, PanelWindowsAreTrailingAndOrdered) {
  const auto bars = synth_gbm_bars(0.25, 300, 9u);
  const auto p = realized_vol_panel(bars);
  ASSERT_TRUE(p.ok());
  for (double v : p->vol) EXPECT_TRUE(std::isfinite(v) && v > 0.05 && v < 0.60);
}

}  // namespace
}  // namespace atx::vol
```

- [ ] **Step 2: Run to verify failure**

Run: `powershell scripts\atx-build.ps1 build atx-vol-tests` → expected compile FAIL: `atx/vol/realized_vol.hpp` not found.

- [ ] **Step 3: Implement header + source**

Header declares exactly the Produces block above (file-level comment: purpose, "pure functions of inputs, no exceptions", thread-safety: stateless). Source implements per-bar quantities with `o = ln(O_t/C_{t-1})`, `u = ln(H/O)`, `d = ln(L/O)`, `c = ln(C/O)`:

```cpp
// atx-vol/src/realized_vol.cpp (core; validation + panel omitted here, required in code)
namespace {
struct BarTerms { double o, u, d, c; };
}  // namespace

Result<double> realized_vol(std::span<const OhlcBar> bars, RvEstimator est,
                            double annualization) {
  if (bars.size() < 2 || !(annualization > 0.0))
    return Err(ErrorCode::InvalidArgument, "realized_vol: need >=2 bars, annualization>0");
  // validate each bar: 0 < low <= min(open,close) <= max(open,close) <= high, finite
  const std::size_t n = bars.size() - 1;  // terms use previous close
  double sum = 0.0, sum_o = 0.0, sum_o2 = 0.0, sum_c = 0.0, sum_c2 = 0.0, sum_rs = 0.0;
  for (std::size_t i = 1; i < bars.size(); ++i) {
    const BarTerms t{std::log(bars[i].open / bars[i - 1].close),
                     std::log(bars[i].high / bars[i].open),
                     std::log(bars[i].low / bars[i].open),
                     std::log(bars[i].close / bars[i].open)};
    switch (est) {
      case RvEstimator::CloseToClose: { const double r = t.o + t.c; sum += r * r; } break;
      case RvEstimator::Parkinson: { const double hl = t.u - t.d;
        sum += hl * hl / (4.0 * std::log(2.0)); } break;
      case RvEstimator::GarmanKlass: { const double hl = t.u - t.d;
        sum += 0.5 * hl * hl - (2.0 * std::log(2.0) - 1.0) * t.c * t.c; } break;
      case RvEstimator::RogersSatchell:
        sum += t.u * (t.u - t.c) + t.d * (t.d - t.c); break;
      case RvEstimator::YangZhang:
        sum_o += t.o; sum_o2 += t.o * t.o; sum_c += t.c; sum_c2 += t.c * t.c;
        sum_rs += t.u * (t.u - t.c) + t.d * (t.d - t.c); break;
    }
  }
  double var_per_bar = 0.0;
  if (est == RvEstimator::YangZhang) {
    const double nn = static_cast<double>(n);
    const double vo = (sum_o2 - sum_o * sum_o / nn) / (nn - 1.0);
    const double vc = (sum_c2 - sum_c * sum_c / nn) / (nn - 1.0);
    const double vrs = sum_rs / nn;
    const double k = 0.34 / (1.34 + (nn + 1.0) / (nn - 1.0));
    var_per_bar = vo + k * vc + (1.0 - k) * vrs;
  } else {
    var_per_bar = sum / static_cast<double>(n);
  }
  if (!(var_per_bar >= 0.0) || !std::isfinite(var_per_bar))
    return Err(ErrorCode::InvalidArgument, "realized_vol: non-finite variance");
  return Ok(std::sqrt(var_per_bar * annualization));
}
```

(YangZhang needs `n >= 3`; return `Err` below that.)

- [ ] **Step 4: Wire into CMake and run tests to green**

Append `src/realized_vol.cpp` to the atx-vol source list; append test file in `tests/CMakeLists.txt` with `# THEO-1 — appended target: realized-vol estimator suite`. Run: `powershell scripts\atx-build.ps1 build atx-vol-tests` then `scripts\atx-build.ps1 ctest -R RealizedVol`. Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add atx-vol/include/atx/vol/realized_vol.hpp atx-vol/src/realized_vol.cpp \
  atx-vol/tests/realized_vol_test.cpp atx-vol/CMakeLists.txt atx-vol/tests/CMakeLists.txt
git commit -m "feat(vol): add OHLC realized-vol estimator suite (CtC/Parkinson/GK/RS/YZ)"
```

---

### Task 2: Breakeven replay — fixed-σ delta-hedged P&L as a backtest-engine extension

**Files:**
- Create: `atx-vol/include/atx/vol/breakeven.hpp` (public API; types + `bev_replay_pnl` declaration)
- Modify: `atx-vol/src/backtest.cpp` (implementation — new engine entry point at the END of the file, after the step-loop code; reuses the file-local `HedgeLedger` and mirrors the financing/settlement blocks; the step loop itself is NOT edited)
- Test: `atx-vol/tests/breakeven_test.cpp`
- Modify: `atx-vol/tests/CMakeLists.txt` (no library-source CMake change: `src/backtest.cpp` is already in the source list)

**Interfaces:**
- Consumes: `american.hpp` — `american_greeks_al(S,K,T,sigma,r,q,side,opts,need_vega,need_rho,need_charm)` (1 boundary solve with all three needs false → price+delta+gamma), `AlOpts`, `al_fast_opts()`, `Side`; `rates_curve.hpp` — `DividendEvent{ex_date_ns, amount}`.
- Produces (all `namespace atx::vol`):

```cpp
// One close per session, sourced from the day's served surface (or synthetic in tests).
struct BevDayState {
  std::int64_t ts_ns{0};   // session close timestamp (ns)
  double s{0.0};           // underlying close
  double r{0.0};           // rate to expiry (cont. comp)
  double q_eff{0.0};       // effective carry to expiry (borrow+div yield proxy)
};

struct BevSpec {
  double strike{0.0};
  std::int64_t expiry_ns{0};
  Side side{Side::Call};
};

// DESIGNATED INITIALIZERS ONLY. Arity-pinned (5).
struct BevReplayConfig {
  std::optional<AlOpts> al_opts{};   // nullopt -> al_fast_opts()
  double hedge_band{0.0};            // |net delta| below which no rebalance
  bool finance_cash{true};           // accrue cash at r*dt (label default ON)
  bool apply_early_exercise{true};   // long-side optimal exercise (B3 semantics)
  double hedge_slippage_bps{0.0};
};

struct BevReplayResult {
  double pnl{0.0};           // terminal cash, unit contract, mult=1
  double premium{0.0};       // entry premium at trial sigma
  double vega_entry{0.0};
  std::uint16_t n_days{0};
  bool exercised_early{false};
  std::int64_t exercise_ts_ns{0};
};

[[nodiscard]] Result<BevReplayResult> bev_replay_pnl(
    std::span<const BevDayState> path, const BevSpec& spec, double sigma,
    std::span<const DividendEvent> dividends, const BevReplayConfig& cfg = {});
```

Semantics (must be in the header comment): long one unit contract bought at `american_price(σ)` on `path[0]`, delta-hedged to zero at every close with the American delta at the same trial σ (self-consistent convention (a) of the research doc §7.4), cash accrues at r·dt when `finance_cash`, hedge shares receive/pay `DividendEvent` amounts on ex-dates in `(prev, curr]`, expiry settles at intrinsic and liquidates shares at the final close, `T = (expiry_ns − ts_ns)/(365.25·86400e9)`; `path.back().ts_ns` must equal `spec.expiry_ns` (fail-closed, mirrors `backtest.hpp:12-15`).

- [ ] **Step 1: Write the failing tests**

```cpp
// atx-vol/tests/breakeven_test.cpp
#include "atx/vol/breakeven.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>

namespace atx::vol {
namespace {

constexpr std::int64_t kDayNs = 86'400'000'000'000LL;

std::vector<BevDayState> synth_gbm_path(double sigma, std::size_t n_days,
                                        std::uint32_t seed, double s0 = 100.0,
                                        double r = 0.0, double q = 0.0) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> z(0.0, 1.0);
  // dt MUST match the timestamp grid: nodes are 1 CALENDAR day apart and the replay
  // prices with T = ns/(365.25*86400e9), so per-step variance uses 1/365.25. (1/252
  // injects a real ~20% realized-vs-hedged vol gap — found in execution, confirmed by
  // pure-BS replication. Tasks 3-5 reuse this corrected helper.)
  const double dt = 1.0 / 365.25, sq = sigma * std::sqrt(dt);
  std::vector<BevDayState> p;
  p.reserve(n_days + 1);
  double s = s0;
  for (std::size_t i = 0; i <= n_days; ++i) {
    p.push_back(BevDayState{static_cast<std::int64_t>(i) * kDayNs, s, r, q});
    s *= std::exp((r - q - 0.5 * sigma * sigma) * dt + sq * z(rng));
  }
  // Calendar-day grid; expiry is the last node.
  return p;
}

BevSpec atm_call_expiring_at(const std::vector<BevDayState>& p) {
  return BevSpec{p.front().s, p.back().ts_ns, Side::Call};
}

TEST(Breakeven, ReplayFailsClosedWhenExpiryNotLastObservation) {
  auto p = synth_gbm_path(0.2, 60, 5u);
  BevSpec spec = atm_call_expiring_at(p);
  spec.expiry_ns += kDayNs;  // one day past the path
  EXPECT_FALSE(bev_replay_pnl(p, spec, 0.2, {}, {}).ok());
}

TEST(Breakeven, HedgedPnlAtTrueVolIsSmallOnAverage) {
  // 40 paths, 126 days: mean PnL at sigma_true within the Derman-Kamal noise band.
  double sum = 0.0;
  int n_ok = 0;
  double vega0 = 0.0;
  for (std::uint32_t seed = 0; seed < 40; ++seed) {
    const auto p = synth_gbm_path(0.25, 126, 100u + seed);
    const auto r = bev_replay_pnl(p, atm_call_expiring_at(p), 0.25, {}, {});
    ASSERT_TRUE(r.ok()) << seed;
    sum += r->pnl;
    vega0 = r->vega_entry;
    ++n_ok;
  }
  const double mean = sum / n_ok;
  // std(P&L) ~ sqrt(pi/4)*vega*sigma/sqrt(N); mean-of-40 shrinks by sqrt(40).
  const double band = std::sqrt(3.14159265 / 4.0) * vega0 * 0.25 /
                      std::sqrt(126.0) / std::sqrt(40.0) * 4.0;
  EXPECT_LT(std::abs(mean), band);
}

TEST(Breakeven, PnlIsMonotoneDecreasingInEntrySigma) {
  const auto p = synth_gbm_path(0.25, 126, 77u);
  const BevSpec spec = atm_call_expiring_at(p);
  double prev = 1e300;
  for (double sig : {0.10, 0.20, 0.30, 0.45, 0.70}) {
    const auto r = bev_replay_pnl(p, spec, sig, {}, {});
    ASSERT_TRUE(r.ok());
    EXPECT_LT(r->pnl, prev) << sig;
    prev = r->pnl;
  }
}

TEST(Breakeven, LongCheapGammaPathIsProfitable) {
  const auto p = synth_gbm_path(0.40, 126, 11u);  // realizes 40 vol
  const auto r = bev_replay_pnl(p, atm_call_expiring_at(p), 0.20, {}, {});  // paid 20
  ASSERT_TRUE(r.ok());
  EXPECT_GT(r->pnl, 0.0);
}

TEST(Breakeven, DeepItmCallExercisesBeforeLargeDividend) {
  auto p = synth_gbm_path(0.15, 60, 3u, /*s0=*/100.0, /*r=*/0.01);
  BevSpec spec{60.0, p.back().ts_ns, Side::Call};   // deep ITM
  const DividendEvent div{p[30].ts_ns + kDayNs / 2, 5.0};  // huge dividend mid-path
  const auto r = bev_replay_pnl(p, spec, 0.15, std::span(&div, 1), {});
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->exercised_early);
  EXPECT_LT(r->exercise_ts_ns, div.ex_date_ns);
}

TEST(Breakeven, GoldenPathPnlIsPinned) {
  const auto p = synth_gbm_path(0.25, 126, 1234u);
  const auto r = bev_replay_pnl(p, atm_call_expiring_at(p), 0.22, {}, {});
  ASSERT_TRUE(r.ok());
  // Pin the value produced by the first green implementation (determinism gate).
  EXPECT_DOUBLE_EQ(r->pnl, r->pnl);  // replace RHS with literal after first green run
}

}  // namespace
}  // namespace atx::vol
```

- [ ] **Step 2: Run to verify failure** — `powershell scripts\atx-build.ps1 build atx-vol-tests` → FAIL: header not found.

- [ ] **Step 3: Implement the engine entry point**

Location: `atx-vol/src/backtest.cpp`, appended after the existing engine code (include `atx/vol/breakeven.hpp` at the top with the other project includes). Reuse rules:
- Share ledger: instantiate the file-local `HedgeLedger` (backtest.cpp:602) with the single hedge instrument as uid 0 (`get(0u)`/`add(0u, dn)`); apply the same band/slippage/cash-settle ordering as `hedge_daily` step 3 (:699-712) — slippage charged on |trade|·spot, notional settled into cash, then `add`.
- Financing: `cash *= exp(r·dt)` mirrors the engine financing block semantics (:3686-3767); ON by default here (label must be carry-faithful) even though the engine's default is OFF.
- Expiry: settle at intrinsic exactly as :3795-3797, then liquidate hedge shares at the final close.
- Early exercise: file-local pure rule below, placed beside this function with a comment referencing the WS-F F3 exercise-model banner (:523-553) — this is a replay-path-only extension of that model; B3 unification post-lakehouse-merge is sprint-doc residual work.

```cpp
// atx-vol/src/backtest.cpp (appended; core loop — validation/banners per house style)
namespace {
constexpr double kYearNs = 365.25 * 86'400.0 * 1e9;
constexpr std::uint16_t kBevMaxDays = 4000;  // bounded-loop guard (JPL Rule 2)

// Replay-path-only extension of the WS-F F3 exercise model (engine step loop still
// settles at expiry only). B3-equivalent pure rule (feat/backtest-lakehouse
// backtest.hpp:636). Unify after merge.
[[nodiscard]] bool bev_should_exercise_early(double intrinsic, double extension_value,
                                             double threshold) noexcept {
  return intrinsic > 0.0 && std::isfinite(extension_value) && extension_value >= 0.0 &&
         std::isfinite(threshold) && threshold > 0.0 && extension_value < threshold;
}
}  // namespace

Result<BevReplayResult> bev_replay_pnl(std::span<const BevDayState> path,
                                       const BevSpec& spec, double sigma,
                                       std::span<const DividendEvent> dividends,
                                       const BevReplayConfig& cfg) {
  if (path.size() < 2 || path.size() > kBevMaxDays)
    return Err(ErrorCode::InvalidArgument, "bev_replay_pnl: path size out of range");
  if (path.back().ts_ns != spec.expiry_ns)
    return Err(ErrorCode::InvalidArgument, "bev_replay_pnl: expiry not last observation");
  if (!(sigma > 0.0) || !(spec.strike > 0.0))
    return Err(ErrorCode::InvalidArgument, "bev_replay_pnl: non-positive sigma or strike");
  const AlOpts opts = cfg.al_opts.value_or(al_fast_opts());

  BevReplayResult out{};
  HedgeLedger ledger;  // the engine's share ledger; single hedge instrument at uid 0
  double cash = 0.0;
  const auto yrs = [&](std::int64_t ts) {
    return static_cast<double>(spec.expiry_ns - ts) / kYearNs;
  };
  // Entry: 1 solve -> price+delta (+vega for the label record via need_vega=true once).
  auto g0 = american_greeks_al(path[0].s, spec.strike, yrs(path[0].ts_ns), sigma,
                               path[0].r, path[0].q_eff, spec.side, opts,
                               /*need_vega=*/true, /*need_rho=*/false, /*need_charm=*/false);
  if (!g0.ok()) return Err(g0.status());
  out.premium = g0->price;
  out.vega_entry = g0->vega;
  cash -= g0->price;
  {
    const double trade0 = -g0->delta;        // delta-neutral entry
    cash -= trade0 * path[0].s;              // buy/sell the hedge
    ledger.add(0u, trade0);
  }

  for (std::size_t i = 1; i < path.size(); ++i) {
    const auto& prev = path[i - 1];
    const auto& cur = path[i];
    const double dt = static_cast<double>(cur.ts_ns - prev.ts_ns) / kYearNs;
    if (cfg.finance_cash) cash *= std::exp(prev.r * dt);
    for (const auto& d : dividends)          // ex-dates in (prev, cur]
      if (d.ex_date_ns > prev.ts_ns && d.ex_date_ns <= cur.ts_ns)
        cash += ledger.get(0u) * d.amount;

    const double t_rem = yrs(cur.ts_ns);
    const double intrinsic = spec.side == Side::Call
                                 ? std::max(0.0, cur.s - spec.strike)
                                 : std::max(0.0, spec.strike - cur.s);
    if (i + 1 == path.size()) {              // expiry: settle at intrinsic (:3795-3797)
      cash += intrinsic;
      cash += ledger.get(0u) * cur.s;        // liquidate hedge
      break;
    }
    auto g = american_greeks_al(cur.s, spec.strike, t_rem, sigma, cur.r, cur.q_eff,
                                spec.side, opts, false, false, false);
    if (!g.ok()) return Err(g.status());

    if (cfg.apply_early_exercise) {
      double threshold = 0.0;
      if (spec.side == Side::Call) {         // pending dividend before next close
        for (const auto& d : dividends)
          if (d.ex_date_ns > cur.ts_ns && d.ex_date_ns <= path[i + 1].ts_ns)
            threshold += d.amount;
      } else {
        threshold = spec.strike * (1.0 - std::exp(-cur.r * t_rem));
      }
      if (bev_should_exercise_early(intrinsic, g->price - intrinsic, threshold)) {
        cash += intrinsic;
        cash += ledger.get(0u) * cur.s;
        out.exercised_early = true;
        out.exercise_ts_ns = cur.ts_ns;
        out.n_days = static_cast<std::uint16_t>(i);
        out.pnl = cash;
        return Ok(out);
      }
    }
    // Rebalance: hedge_daily's band/slippage/settle/record ordering (:699-712),
    // slippage folded into cash (single-instrument replay has no separate cost lane).
    const double net = g->delta + ledger.get(0u);
    if (std::abs(net) > cfg.hedge_band) {
      const double trade = -net;
      cash -= std::abs(trade) * cur.s * cfg.hedge_slippage_bps / 1e4;
      cash -= trade * cur.s;
      ledger.add(0u, trade);
    }
  }
  out.n_days = static_cast<std::uint16_t>(path.size() - 1);
  out.pnl = cash;
  return Ok(out);
}
```

After implementing: run the FULL existing backtest suite in addition to the new Breakeven suite — the engine file changed, and the addition must be provably inert for every existing path (no step-loop edits, no signature changes, bit-identity untouched).

- [ ] **Step 4: Run tests to green; pin the golden.** Run `scripts\atx-build.ps1 ctest -R Breakeven`. On first green, replace the golden test's RHS with the printed literal and re-run.

- [ ] **Step 5: Commit**

```bash
git add atx-vol/include/atx/vol/breakeven.hpp atx-vol/src/backtest.cpp \
  atx-vol/tests/breakeven_test.cpp atx-vol/tests/CMakeLists.txt
git commit -m "feat(vol): breakeven replay entry point in backtest engine (fixed-sigma delta-hedged PnL, early exercise)"
```

---

### Task 3: Breakeven root-find (`solve_breakeven_vol`)

**Files:**
- Modify: `atx-vol/include/atx/vol/breakeven.hpp`
- Create: `atx-vol/src/breakeven.cpp` (root-find/batch/loader layer over the engine's `bev_replay_pnl`; append to the atx-vol source list in `atx-vol/CMakeLists.txt` with this task)
- Test: `atx-vol/tests/breakeven_test.cpp` (append)

**Interfaces:**
- Consumes: Task 2's `bev_replay_pnl`, `BevDayState`, `BevSpec`, `BevReplayConfig`.
- Produces:

```cpp
// DESIGNATED INITIALIZERS ONLY. Arity-pinned (4).
struct BevSolveConfig {
  BevReplayConfig replay{};
  double sigma_lo{0.01};
  double sigma_hi{3.00};
  double sigma_tol{1e-4};
};

enum class BevFlag : std::uint8_t { Ok = 0, NoBracket = 1, ExercisedEarly = 2,
                                    MaxIter = 3 };

struct BevLabel {
  double sigma_be{0.0};
  double premium_at_be{0.0};
  double vega_at_be{0.0};
  double pnl_residual{0.0};
  std::uint16_t n_days{0};
  std::uint8_t iters{0};
  BevFlag flag{BevFlag::Ok};
};

[[nodiscard]] Result<BevLabel> solve_breakeven_vol(std::span<const BevDayState> path,
                                                   const BevSpec& spec,
                                                   std::span<const DividendEvent> dividends,
                                                   const BevSolveConfig& cfg = {});
```

Algorithm: PnL(σ) is monotone decreasing (Task 2 test). Evaluate at `sigma_lo`/`sigma_hi`; no sign change → `Ok(label{flag=NoBracket})` (wing ill-conditioning is data, not an error). Bisection, `kBevMaxSolveIter = 40` bounded; converge on `sigma_tol`; record final residual and iteration count. `ExercisedEarly` flag set if the converged replay exercised early.

- [ ] **Step 1: Append failing tests**

```cpp
TEST(Breakeven, SolveRecoversTrueVolOnGbmWithinNoiseBand) {
  // sigma_be estimates gamma-weighted realized vol; across seeds it centers on 0.25.
  double sum = 0.0;
  for (std::uint32_t seed = 0; seed < 20; ++seed) {
    const auto p = synth_gbm_path(0.25, 126, 500u + seed);
    const auto lab = solve_breakeven_vol(p, atm_call_expiring_at(p), {}, {});
    ASSERT_TRUE(lab.ok());
    ASSERT_EQ(lab->flag, BevFlag::Ok);
    sum += lab->sigma_be;
  }
  EXPECT_NEAR(sum / 20.0, 0.25, 0.02);
}

TEST(Breakeven, SolveResidualIsWithinVegaScaledTolerance) {
  const auto p = synth_gbm_path(0.30, 126, 900u);
  const auto lab = solve_breakeven_vol(p, atm_call_expiring_at(p), {}, {});
  ASSERT_TRUE(lab.ok());
  EXPECT_LT(std::abs(lab->pnl_residual), lab->vega_at_be * 2e-4 + 1e-8);
}

TEST(Breakeven, FarOtmWingReturnsNoBracketNotError) {
  const auto p = synth_gbm_path(0.10, 21, 8u);
  BevSpec spec{p.front().s * 3.0, p.back().ts_ns, Side::Call};  // absurd wing
  const auto lab = solve_breakeven_vol(p, spec, {}, {});
  ASSERT_TRUE(lab.ok());
  EXPECT_EQ(lab->flag, BevFlag::NoBracket);
}
```

- [ ] **Step 2: Run to verify failure** (`solve_breakeven_vol` undefined).
- [ ] **Step 3: Implement bisection with bounded iterations; store residual/iters/flag.**
- [ ] **Step 4: Run to green** — `scripts\atx-build.ps1 ctest -R Breakeven`.
- [ ] **Step 5: Commit** — `feat(vol): breakeven-vol root-find with NoBracket wing handling`

---

### Task 4: Batch label runner — deterministic parallel fan-out

**Files:**
- Modify: `atx-vol/include/atx/vol/breakeven.hpp`, `atx-vol/src/breakeven.cpp`
- Test: `atx-vol/tests/breakeven_test.cpp` (append)
- Create: `atx-vol/bench/theo_bench.cpp`; Modify: `atx-vol/bench/CMakeLists.txt` (new target `atx-vol-theo-bench` following the per-domain pattern)

**Interfaces:**
- Consumes: Task 3's `solve_breakeven_vol`; `detail/parallel_for.hpp` (contiguous `[lo,hi)` per worker, disjoint writes → bit-identical for any `n_threads`).
- Produces:

```cpp
struct BevJob {                       // one label request
  std::span<const BevDayState> path;  // non-owning; caller keeps alive
  BevSpec spec{};
  std::span<const DividendEvent> dividends;
};

struct BevLabelFrame {                // SoA results, index-aligned with jobs
  std::vector<double> sigma_be, premium_at_be, vega_at_be, pnl_residual;
  std::vector<std::uint16_t> n_days;
  std::vector<std::uint8_t> iters, flag;   // flag = BevFlag
  std::vector<std::uint8_t> status_ok;     // 1 = solver ran, 0 = input rejected
};

[[nodiscard]] Result<BevLabelFrame> solve_breakeven_batch(std::span<const BevJob> jobs,
                                                          const BevSolveConfig& cfg = {},
                                                          unsigned n_threads = 0);
```

Per-job failure writes `status_ok=0` and continues (a bad job must not sink the batch). `n_threads` semantics mirror `parallel_for` (0=auto, 1=serial byte-for-byte).

- [ ] **Step 1: Append failing tests** — (a) batch of 32 synthetic jobs equals 32 serial `solve_breakeven_vol` calls field-for-field; (b) `n_threads=1` vs `n_threads=4` frames are bit-identical (`EXPECT_EQ` on every vector); (c) one poisoned job (empty path) yields `status_ok[j]==0` while neighbors solve.

```cpp
TEST(Breakeven, BatchMatchesSerialFieldForField) { /* build 32 jobs from seeds 0..31,
  run solve_breakeven_batch(jobs, {}, 1) and per-job solve_breakeven_vol; EXPECT_EQ all */ }
TEST(Breakeven, BatchIsBitIdenticalAcrossThreadCounts) { /* frames t1 vs t4; memcmp-style */ }
TEST(Breakeven, PoisonedJobDoesNotSinkBatch) { /* jobs[7].path = {}; status_ok[7]==0 */ }
```

(Write these as real tests with the Task 2 `synth_gbm_path` helper — the skeleton comments above are for plan brevity; the test bodies follow the Step-1 pattern of Tasks 2/3 exactly.)

- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement with `detail::parallel_for`;** reserve all frame vectors up front; workers write disjoint indices only.
- [ ] **Step 4: Run to green, including the forcescalar lane** (`ATX_SIMD_ISA=ForceScalar` re-run if the suite lands in the pinned lane).
- [ ] **Step 5: Add bench** — `theo_bench.cpp`: one benchmark `BM_BevSolve_126d_AlFast` (single label, 126-day path) and `BM_BevBatch_64jobs`. Record ns/label; anchor note per `bench/ANCHORS.md` (cite CPU; expect ~0.2-0.3 s/label at al_fast per the perf review).
- [ ] **Step 6: Commit** — `feat(vol): deterministic parallel breakeven label batch + bench`

---

### Task 5: Path loader — real surfaces to `BevDayState`

**Files:**
- Modify: `atx-vol/include/atx/vol/breakeven.hpp`, `atx-vol/src/breakeven.cpp`
- Test: `atx-vol/tests/breakeven_test.cpp` (append; suite added to `ATX_VOL_SLOW_FILTER` if fixture-bound)

**Interfaces:**
- Consumes: `backtest.hpp` — `Clock` (`from_surface_db`, `between`), `SnapshotRef`; `session.hpp`/`MarketSnapshot::load(path)` + `find(uid) -> SurfaceRef`; `priced_surface.hpp` — `pricing().S`, `pricing().r`, `q_eff_at(T)`.
- Produces:

```cpp
enum class BevExpirySnap : std::uint8_t { Exact = 0, LastSessionAtOrBefore = 1 };

struct BevPath {                       // owning day-state path + provenance
  std::vector<BevDayState> days;
  std::int64_t settle_ts_ns{0};        // == spec.expiry_ns unless snapped
  bool snapped{false};
};

[[nodiscard]] Result<BevPath> load_bev_path(const Clock& clock, std::string_view uid,
                                            std::int64_t entry_ts_ns,
                                            std::int64_t expiry_ns, double tenor_probe_years,
                                            BevExpirySnap snap = BevExpirySnap::Exact);
```

Per day: `S = pricing().S`, `r = pricing().r`, `q_eff = q_eff_at(max(t_rem, kTMinEval))` where `t_rem` is remaining tenor from that session to `expiry_ns` (per-day carry — the review's "carry errors masquerade as skew" warning made concrete). `Exact` fails closed when `expiry_ns` is not a clock observation; `LastSessionAtOrBefore` snaps and sets `snapped=true` (the label consumer decides whether snapped labels are admissible). `tenor_probe_years` guards the `q_eff_at` floor near expiry.

- [ ] **Step 1: Append failing tests** using the Tier-B `spy_fixture.hpp` corpus: (a) loader returns one `BevDayState` per session between entry and expiry with strictly increasing `ts_ns` and positive spots; (b) `Exact` snap on a non-session expiry fails closed; (c) `LastSessionAtOrBefore` returns `snapped=true` and `settle_ts_ns < expiry_ns`; (d) end-to-end: `load_bev_path` + `solve_breakeven_vol` on one SPY contract produces `BevFlag::Ok` and `sigma_be` within `[0.05, 1.0]`. Follow the fixture-usage pattern of the existing tests that consume `spy_fixture.hpp`.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** (uses `MarketSnapshot::load` per ref; no `SnapshotCache` needed at this layer — the driver batches by date to amortize archive opens, Task 6).
- [ ] **Step 4: Run to green.**
- [ ] **Step 5: Commit** — `feat(vol): BEV path loader from surface corpora with fail-closed expiry snap`

---

### Task 6: Label-factory driver (`examples/bev_label_factory.cpp`)

**Files:**
- Create: `atx-vol/examples/bev_label_factory.cpp`
- Test: `atx-vol/tests/bev_label_factory_gate_test.cpp` (self-contained gate, mirrors `tests/spy_strangle_backtest_test.cpp` pattern)
- Modify: `atx-vol/CMakeLists.txt` (example block under `ATX_BUILD_EXAMPLES` with why-comment), `atx-vol/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 3-5 (`load_bev_path`, `solve_breakeven_batch`), `SurfaceDb::open/partitions`, `Clock::from_surface_db`, `write` conventions from `tools/tearsheet.hpp` TSV writers (`# key=value` metadata header).
- Produces: CLI `bev_label_factory --db <root> --uid <symbol> --entry-start <date> --entry-end <date> --tenor-days <n> --delta-lo 0.05 --delta-hi 0.95 --dividends <tsv> --out labels.tsv`. Output TSV columns: `entry_ts_ns uid strike expiry_ns side sigma_be sigma_entry_iv log_ratio premium vega n_days iters flag snapped` (event counts join Python-side at training time from the earnings calendar; the driver stays carry-only). `sigma_entry_iv` is the served surface `iv(K,T)` at entry; `log_ratio = ln(sigma_be / sigma_entry_iv)` (the model target from the research doc §8.2). Rows sorted by `(entry_ts_ns, expiry_ns, strike, side)` — deterministic byte-stable output. Strike lattice: per entry date, strikes whose entry |delta| ∈ [delta-lo, delta-hi] on the served surface (wing ill-conditioning filter, research §7.4).

- [ ] **Step 1: Write the failing gate test** — drives the driver logic as a library-level function (extract `Result<int> run_bev_label_factory(const BevFactoryArgs&)` into the example TU with a small header-free arg struct so the gate test can call it): runs twice over the SPY fixture with identical args into two temp files, asserts (a) exit 0, (b) both files byte-identical, (c) ≥ 1 row with `flag==0`, (d) every row's `log_ratio` finite.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement driver** — hand-rolled argv loop (house pattern, cf. `spy_leaps_strangle_backtest.cpp:80-136`); group label jobs by entry date; load each session's snapshot once; dividends TSV loader reuses the `earnings_forecast_loader` TSV-parsing style (epoch-ns + amount per line, `#` comments skipped).
- [ ] **Step 4: Run gate to green.**
- [ ] **Step 5: Commit** — `feat(vol): bev_label_factory driver with byte-deterministic TSV output`

---

### Task 7: `theo.hpp` vocabulary + `TheoEngine` with identity semantics

**Files:**
- Create: `atx-vol/include/atx/vol/theo.hpp`
- Create: `atx-vol/src/theo.cpp`
- Test: `atx-vol/tests/theo_test.cpp`
- Modify: `atx-vol/CMakeLists.txt`, `atx-vol/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `priced_surface.hpp` — `PricedSurface` (`iv(K,T)`, `fair_value(K,T,side)`, `greeks_analytic`, `forward_at`, `pricing()`); `event_vol.hpp` — `EventSchedule`; Task 1 `RvPanel`.
- Produces (all `namespace atx::vol`):

```cpp
struct TheoQuery { double strike{0.0}; double tenor_years{0.0}; Side side{Side::Call}; };

enum class TheoFlagBits : std::uint32_t {
  None = 0, Extrapolated = 1u << 0, OverlayClamped = 1u << 1, ModelMissing = 1u << 2,
};

struct TheoValue {
  double theo_vol{0.0};      // de-Americanized vol space, same space as surface iv()
  double theo_price{0.0};    // American premium at theo_vol (surface carry inputs)
  double market_vol{0.0};    // served surface iv(K,T)
  double market_price{0.0};  // served surface fair_value
  double edge_vol{0.0};      // market_vol - theo_vol  (>0 => market rich vs theo)
  double band_vol{0.0};      // half-width uncertainty band on theo_vol
  std::uint32_t flags{0};
};

// Non-owning market-state bundle (SurfaceSet convention: caller keeps alive).
struct TheoContext {
  const PricedSurface* surface{nullptr};       // required
  const EventSchedule* events{nullptr};        // optional
  const RvPanel* rv{nullptr};                  // optional
};

struct OverlayAdjust { double dvol{0.0}; double band{0.0}; };

class ITheoOverlay {
 public:
  virtual ~ITheoOverlay() = default;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  // Additive vol-space adjustment per query. Batch-first; scalar callers wrap n=1.
  [[nodiscard]] virtual Status adjust(const TheoContext& ctx,
                                      std::span<const TheoQuery> queries,
                                      std::span<OverlayAdjust> out) const = 0;
};

// DESIGNATED INITIALIZERS ONLY. Arity-pinned (3).
struct TheoConfig {
  double band_floor_vol{0.002};   // minimum band: label-noise floor (Derman-Kamal)
  double max_abs_dvol{0.15};      // per-overlay clamp; clamping sets OverlayClamped
  bool price_theo{true};          // false: skip American reprice (vol-space-only sheet)
};

class TheoEngine {
 public:
  static Result<TheoEngine> create(std::vector<std::unique_ptr<ITheoOverlay>> overlays,
                                   const TheoConfig& cfg = {});
  [[nodiscard]] Result<TheoValue> value(const TheoContext& ctx, const TheoQuery& q) const;
  [[nodiscard]] Status value_into(const TheoContext& ctx, std::span<const TheoQuery> qs,
                                  std::span<TheoValue> out) const;  // caller-owned out
 private:
  explicit TheoEngine(std::vector<std::unique_ptr<ITheoOverlay>> ovs, const TheoConfig& c);
  std::vector<std::unique_ptr<ITheoOverlay>> overlays_;
  TheoConfig cfg_;
};
```

Semantics: `theo_vol = market_vol + Σ clamp(overlay dvol)`; `band_vol = max(band_floor_vol, sqrt(Σ band²))`; `theo_price` = `american_price` at `theo_vol` with the surface's own carry (`forward_at`/`q_eff_at`/`pricing().r/method/al_opts`) so **zero overlays ⇒ theo ≡ market bit-for-bit** — the identity contract that keeps theo an overlay measure, never a competing mark.

- [ ] **Step 1: Write failing tests** — (a) `EngineWithNoOverlaysReproducesSurfaceExactly`: on the SPY fixture, for a (K,T) grid: `theo_vol == iv(K,T)` (EXPECT_DOUBLE_EQ), `theo_price == *fair_value(K,T,side)`, `edge_vol == 0.0`; (b) `NullSurfaceIsRejected`; (c) `OutSpanSizeMismatchIsRejectedBeforeMutation` (value_into with short out span leaves out untouched); (d) stub overlay adding +0.02 shifts `theo_vol` by exactly 0.02 and sets `edge_vol = -0.02`; (e) overlay dvol beyond `max_abs_dvol` clamps and sets `OverlayClamped`.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** (engine core is ~150 lines; per-query surface reads batch through `value_into`'s single loop; no allocation in `value_into` — overlay scratch is a caller-invisible fixed `std::array<OverlayAdjust, kTheoMaxBatch>` chunk loop, `kTheoMaxBatch = 256`).
- [ ] **Step 4: Run to green.**
- [ ] **Step 5: Commit** — `feat(vol): theo vocabulary + TheoEngine with exact identity semantics`

---

### Task 8: First real overlays — RV-blend fair vol + event variance

**Files:**
- Modify: `atx-vol/include/atx/vol/theo.hpp`, `atx-vol/src/theo.cpp`
- Test: `atx-vol/tests/theo_test.cpp` (append)

**Interfaces:**
- Consumes: Task 7 interfaces; `event_vol.hpp` — `censored_total_variance(w_total, n_events, emove)`, `event_recombined_vol(atm_cen, T, n, emove)`, `EventSchedule::count_between`.
- Produces:

```cpp
// theo vol level lean: pull ATM level toward an RV-anchored forecast, damped in tenor.
// DESIGNATED INITIALIZERS ONLY. Arity-pinned (3).
struct RvBlendConfig {
  double weight{0.35};        // 0 = identity; 1 = full RV anchor at the short end
  double tenor_damp_years{1.0};  // weight *= exp(-tenor/tenor_damp_years)
  std::uint8_t rv_window_idx{1};  // RvPanel window used as anchor (default 21d)
};
[[nodiscard]] Result<std::unique_ptr<ITheoOverlay>> make_rv_blend_overlay(RvBlendConfig cfg = {});

// event variance swap: strip the market's implied event move, re-inject our own forecast.
// DESIGNATED INITIALIZERS ONLY. Arity-pinned (2).
struct EventVarConfig {
  double emove_forecast{0.0};    // our per-event daily move forecast (0 disables)
  double emove_market{0.0};      // market-implied move to strip (from implied_emove_joint)
};
[[nodiscard]] Result<std::unique_ptr<ITheoOverlay>> make_event_var_overlay(EventVarConfig cfg);
```

RvBlend math (research doc §6.2 — IV is the strongest single RV predictor but premium-biased; the blend is the standard desk fair-vol core): `dvol = w(T) · (rv_anchor − market_vol)` where `w(T) = weight · exp(−T/tenor_damp_years)`; requires `ctx.rv`, else `ModelMissing` flag with `dvol=0`. EventVar math (research §6.3, existing machinery): with `n = events->count_between(now, expiry)`, strip via `censored_total_variance(market_vol²·T, n, emove_market)` then re-inject `event_recombined_vol(atm_cen, T, n, emove_forecast)`; `dvol = recombined − market_vol`; requires `ctx.events`, else `ModelMissing`.

- [ ] **Step 1: Append failing tests** — (a) `RvBlendWeightZeroIsIdentity`; (b) known arithmetic: market 0.30, rv anchor 0.20, weight 0.5, T→0 ⇒ dvol = −0.05 (EXPECT_NEAR 1e-12); (c) tenor damping monotone: |dvol| decreasing in T; (d) missing `ctx.rv` sets `ModelMissing`, edge 0; (e) EventVar with `emove_forecast == emove_market` is identity (within 1e-12); (f) EventVar with forecast < market lowers theo vol only for expiries containing the event (`count_between > 0`).
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement both overlays** (each ~60 lines; pure over ctx+queries).
- [ ] **Step 4: Run to green.**
- [ ] **Step 5: Commit** — `feat(vol): RV-blend and event-variance theo overlays`

---

### Task 9: The ML seam — `IFairVolModel` + breakeven-model overlay

**Files:**
- Modify: `atx-vol/include/atx/vol/theo.hpp`, `atx-vol/src/theo.cpp`
- Test: `atx-vol/tests/theo_test.cpp` (append)

**Interfaces:**
- Consumes: Task 7 interfaces; Task 1 `RvPanel`.
- Produces:

```cpp
// Feature vector contract for fair-vol models. Fixed order, versioned; the label
// factory (Task 6 TSV) and any offline trainer must produce/consume this exact layout.
inline constexpr std::size_t kFairVolFeatureCount = 8;
inline constexpr std::uint32_t kFairVolFeatureSchemaV1 = 1;
// [0] log_moneyness = ln(K/F)      [1] tenor_years
// [2] market_vol                   [3] rv_21d
// [4] rv_63d                       [5] iv_minus_rv = market_vol - rv_21d
// [6] n_events_to_expiry           [7] delta_abs (surface analytic |delta|)

class IFairVolModel {
 public:
  virtual ~IFairVolModel() = default;
  [[nodiscard]] virtual std::uint32_t feature_schema() const noexcept = 0;
  // Predicts y = ln(sigma_fair / market_vol) per row. Batch-first.
  [[nodiscard]] virtual Status predict(std::span<const double> features_row_major,
                                       std::size_t n_rows,
                                       std::span<double> log_ratio_out) const = 0;
};

// v1 model: linear on the fixed schema, coefficients loaded from a TSV
// ("# schema=1" header line; kFairVolFeatureCount+1 whitespace-separated values:
//  intercept then one coefficient per feature).
[[nodiscard]] Result<std::unique_ptr<IFairVolModel>> load_linear_fair_vol_model(
    std::string_view coef_tsv_path);

[[nodiscard]] Result<std::unique_ptr<ITheoOverlay>> make_fair_vol_model_overlay(
    std::shared_ptr<const IFairVolModel> model);
```

The overlay assembles the feature block from `ctx` (surface + rv + events), calls `predict`, and emits `dvol = market_vol · (exp(y) − 1)`; band contribution `|dvol| · 0.5` until the model ships quantile heads (recorded as residual work). Missing `ctx.rv`/`ctx.events` ⇒ `ModelMissing` flag, `dvol = 0` (fail-open to identity, never fail-closed — theo must always serve).

- [ ] **Step 1: Append failing tests** — (a) zero-coefficient model ⇒ engine identity; (b) intercept-only model with `b0 = ln(0.9)` ⇒ `theo_vol == 0.9 · market_vol` (EXPECT_NEAR 1e-10); (c) schema mismatch (`# schema=2`) refused at load; (d) malformed coef file (wrong count) refused with `ParseError`-family code; (e) missing rv panel ⇒ `ModelMissing`, edge 0.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** loader (TSV, `#` comment skip, exact count check), model (dot product, `noexcept` inner loop), overlay (feature assembly + prediction + conversion).
- [ ] **Step 4: Run to green.**
- [ ] **Step 5: Commit** — `feat(vol): IFairVolModel seam with linear v1 loader and model overlay`

---

### Task 10: Batch perf pass + backtest edge signals

**Files:**
- Modify: `atx-vol/src/theo.cpp` (perf), `atx-vol/bench/theo_bench.cpp` (append `BM_TheoSheet_200q`)
- Create: theo signal probe inside `atx-vol/examples/spy_leaps_strangle_backtest.cpp` (modify) — no library change
- Test: `atx-vol/tests/theo_test.cpp` (append)

**Interfaces:**
- Consumes: `IStrategy::signals(const MarketSnapshot&) -> std::vector<std::pair<std::string, double>>` (`strategy.hpp:524-529`; `SwapSignalProbe` is the worked precedent); Task 7 `TheoEngine::value_into`.
- Produces:
  - `[[nodiscard]] Result<std::vector<TheoValue>> compute_theo_sheet(const TheoContext& ctx, const TheoEngine& engine, std::span<const TheoQuery> queries);` — allocating convenience over `value_into` (the `compute_surface_analytics` shape).
  - In the LEAPS example: a wrapper strategy decorating the existing strangle strategy whose `signals()` emits `theo_edge_atm` (edge_vol at the ATM tenor-matched query) and `theo_band_atm` per step, built from a `TheoEngine{RvBlend}` over the step's snapshot surface.

- [ ] **Step 1: Append failing tests** — (a) `ValueIntoDoesNotAllocatePerQuery`: 10k-query `value_into` into preallocated span succeeds (allocation assertion via counting overlay calls per chunk — verify chunking math: `ceil(10000/256)` overlay batch calls exactly); (b) `SheetMatchesValueIntoFieldForField`.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement + bench.** Record `BM_TheoSheet_200q` with `price_theo=true` and `false` (the American reprice dominates; vol-space-only sheets are the cheap screening path). Compare against anchors per `bench/ANCHORS.md` rules.
- [ ] **Step 4: Wire the example signal probe; run the LEAPS driver; verify** (a) signal columns appear in `track.tsv`, (b) NAV byte-identical to the no-probe run (signals are read-only — this is the non-negotiable integration proof; mirrors the swap-probe precedent).
- [ ] **Step 5: Run full atx_vol_fast lane green** — `scripts\atx-build.ps1 ctest -L atx_vol_fast`.
- [ ] **Step 6: Commit** — `feat(vol): theo sheet batch path, bench, and read-only backtest edge signals`

---

### Task 11: Sprint closeout — docs, hygiene, CHANGELOG

**Files:**
- Create: `atx-vol/sprints/2026-08-XX-theo-module-sprint-summary.md`
- Modify: `atx-vol/CHANGELOG.md` (follow existing entry style), `atx-vol/docs/` index if one exists at execution time

**Interfaces:** none (documentation).

- [ ] **Step 1: Run the hygiene preset** (`scripts\atx-build.ps1` hygiene lane) — include-cleanliness gate for the three new modules; fix any PCH-masked missing includes.
- [ ] **Step 2: Run the full test gate** (fast + slow labels + forcescalar lane) and the two bench targets; capture numbers.
- [ ] **Step 3: Write the sprint summary** — shipped surface (files + public API), measured perf (ns/label, labels/hour extrapolation, theo sheet throughput), and the **residual-work register** (house convention — these are deliberately NOT code TODOs): B3 rule unification post-lakehouse-merge; quantile heads on `IFairVolModel` (band from model, not the 0.5·|dvol| placeholder); dividend/borrow inputs for single names (corporate-actions store is Python-side; C++ consumer is atx-engine only); label storage beyond TSV (Parquet deferral is a house rule — revisit with the lakehouse); event-days-excluded label variant (research §8.2 item 4); purged-CV/embargo tooling lives Python-side with the trainer, out of C++ scope.
- [ ] **Step 4: Commit** — `docs(vol): theo module sprint summary + changelog`

---

## Execution order & dependency graph

```
Task 1 (realized_vol) ──────────────┐
Task 2 (replay kernel) → Task 3 (root-find) → Task 4 (batch) → Task 6 (driver)
                                    │                              ↑
                                    └── Task 5 (path loader) ──────┘
Task 7 (theo core) → Task 8 (overlays; needs Task 1) → Task 9 (model seam)
Task 7..9 → Task 10 (perf + signals) → Task 11 (closeout)
```

Tasks 1, 2, and 7 are independent starting points (three-lane parallel start is safe: disjoint files).

## Self-review notes

- Spec coverage: research-doc system stages → label factory (Tasks 2-6), fair-vol overlays (8), ML seam (9), edge-into-backtest (10). Trading/portfolio layer is explicitly out of scope for this sprint (no positions are taken; signals only) — recorded in the closeout register.
- Type consistency: `BevDayState/BevSpec/BevReplayConfig/BevReplayResult` (Task 2) consumed verbatim by Tasks 3-6; `TheoQuery/TheoValue/TheoContext/ITheoOverlay` (Task 7) consumed verbatim by Tasks 8-10; `RvPanel` (Task 1) consumed by Tasks 8-9.
- Known judgment calls an executor must respect: (i) `ErrorCode::InvalidArgument` — if atx-core's enum lacks it, use the domain-validation code american.hpp uses at its entry guards (grep one call site before first use); (ii) the Task 2 golden literal is pinned on first green run by design; (iii) `scripts\atx-build.ps1` lane names — use the repo's actual flag spelling from the script header if it differs from the shorthand here.
