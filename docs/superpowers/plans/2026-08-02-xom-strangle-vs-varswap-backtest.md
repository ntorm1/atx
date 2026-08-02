# XOM Strangle-vs-VarSwap Comparison Backtest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A backtest that compares holding a daily-restruck 40-delta 3M XOM strangle (delta-neutral) against holding an uncapped 3M variance swap, equal vega at inception, projected onto every historical XOM surface for 2026, with PnL and greeks compared through time.

**Architecture:** A new `StrangleVsVarswapStrategy` (C++ `IStrategy` subclass) composes both legs inside one `run_backtest` run: the option leg is a single-name 40Δ strangle at a **fixed snapped ~3M expiry**, restruck to fresh 40Δ strikes every step (close + reopen at the same expiry) and delta-hedged daily via the engine's hedge-share ledger; the swap leg is one uncapped `VarSwap` `SwapLot` opened at cycle start at the surface fair strike with `qty` solved so its `deriv_greeks` vega equals the strangle's entry vega. When the expiry is reached both legs settle and a new cycle opens at the next session. Swap greeks are emitted per step through `IStrategy::signals()` (the engine's swap lane computes price only). A thin C++ driver binds it to the on-disk SP100 surface DB; a small python renderer overlays the two legs' pnl/greek series.

**Tech Stack:** C++20 (clang-cl 18, /W4 /permissive- /WX), GoogleTest, existing atx-vol backtest engine + vol-derivatives lane, python 3 + matplotlib for the report.

## Semantics locked by this plan (user-facing decisions, surfaced for review)

- "Fixed expiry, restrike every day": within a cycle the expiry is FIXED (first synthetic 3M expiry snapped to the session grid); only the strangle's strikes roll daily to 40Δ. The var swap is untouched within a cycle (fixings accrue).
- "Equal vega": matched once per cycle at inception (swap qty solved against strangle entry vega). Restrikes do not re-scale either leg.
- "For 2026": every session the DB holds for XOM in 2026 (verify coverage in Task 5; cycles roll at each expiry until the run window ends; final cycle liquidates at run end via the engine's normal end-of-run handling).
- Frictionless v1: no option spread/impact costs, no swap entry cost (the swap lane is zero-cost by design).
- Strangle size: fixed `contracts` per leg (config, default 100); swap qty is the free variable.

## Global Constraints

- Coding standard: `C:\atx\.agents\cpp\agent.md` binds all C++ (TDD RED→GREEN mandatory, Result<T>/Status error model, NaN-never-0.0 for unavailable values, /W4 /WX clean, explicit CMake source lists).
- Build/test: `powershell scripts\atx-build.ps1 build <target>` / `-Ctest -R <regex>` (separate `-R` invocations, never `|` alternation), powershell 5.1.
- Engine invariants that bind this feature: `SwapLot` is immutable once opened, no early close (v1), additions allowed any step with ids in `[next_id_before, next_id_after)` (`validate_swap_transition`, backtest.cpp:821-882); `RunConfig::unpriced = UnpricedLotPolicy::ExcludeAndReport` for hedge trading; swap lane computes price only — greeks must come from `signals()`.
- New test files join the explicit lists in `atx-vol/tests/CMakeLists.txt` under a `# strangle-vs-varswap` comment; suites must land in the `atx_vol_fast` label (<10 s) else append to `ATX_VOL_SLOW_FILTER`.
- Namespace `atx::vol`; headers under `atx-vol/include/atx/vol/`; sources under `atx-vol/src/`.
- Surface DB root for the real run: `C:/atx-data/surface-db/sp100-2026` (XOM is in `atx-vol/data/universe/sp100_2026-07.csv:17`).

## File Structure

- `atx-vol/include/atx/vol/strangle_varswap.hpp` — config struct + strategy class declaration + spec-assembly helper (one responsibility: the comparison strategy).
- `atx-vol/src/strangle_varswap.cpp` — implementation.
- `atx-vol/tests/strangle_varswap_test.cpp` — all strategy tests (Tasks 1-3).
- `atx-vol/examples/strangle_varswap_driver.cpp` — CLI driver executable (Task 4).
- `atx-vol/tools/render_strangle_vs_varswap.py` — report renderer (Task 4).
- `atx-vol/python/tests/test_render_strangle_vs_varswap.py` — renderer test (Task 4).
- Task 5 produces run artifacts + docs rows (README/CHANGELOG).

---

### Task 1: Strangle leg — fixed-expiry daily restrike strategy (options only)

**Files:**
- Create: `atx-vol/include/atx/vol/strangle_varswap.hpp`
- Create: `atx-vol/src/strangle_varswap.cpp`
- Test: `atx-vol/tests/strangle_varswap_test.cpp`
- Modify: `atx-vol/CMakeLists.txt`, `atx-vol/tests/CMakeLists.txt` (explicit lists)

**Interfaces:**
- Consumes: `IStrategy` contract (strategy.hpp:419-486), `resolve_strike_by_delta` (strategy.hpp:269-278), `TenorSpec::snap_to_sessions` (strategy.hpp:63-74), `HedgeSpec` (strategy.hpp:164-170), lot open/close conventions from `PortfolioState` (backtest.hpp:400-420). Study `examples/strategy_examples.cpp:200-221` (hand-built single-name spec) and `DeclarativeStrategy` (strategy.hpp:490-541) before writing code — follow their conventions for lot ids, expiry snapping, and hedge_spec.
- Produces (Tasks 2-4 rely on these exact names):

```cpp
namespace atx::vol {

struct StrangleVarswapConfig {
  std::string symbol = "XOM";
  double target_abs_delta = 0.40;   // strangle wing delta
  double tenor_years = 0.25;        // ~3M; snapped to the session grid
  double contracts = 100.0;         // strangle qty per wing (fixed)
  std::vector<std::int64_t> session_ts; // snap grid, driver-supplied
  // Task 2 adds: bool enable_swap_leg = true; DerivConfig deriv_cfg;
};

class StrangleVsVarswapStrategy final : public IStrategy {
 public:
  explicit StrangleVsVarswapStrategy(StrangleVarswapConfig cfg);
  // IStrategy overrides per strategy.hpp:419-486 (on_step, hedge_spec, signals).
 private:
  StrangleVarswapConfig cfg_;
  std::int64_t cycle_expiry_ts_ns_ = 0; // 0 = no live cycle
  // per-cycle bookkeeping added as needed
};

} // namespace atx::vol
```

Behavior contract (all tested):
1. First step: pick cycle expiry = first snapped session ≥ step_ts + tenor_years (reuse the same snapping helper `DeclarativeStrategy` uses; if the grid ends before step_ts + tenor, use the LAST session — final short cycle). Open one call + one put at ±40Δ strikes resolved on the step's surface, both at the cycle expiry, qty = `contracts`.
2. Every subsequent step while cycle live and expiry not reached: close both option lots, reopen at freshly resolved ±40Δ strikes, SAME expiry, same qty.
3. Step at/past expiry: let the engine settle the expiring lots; open the next cycle (new fixed expiry) on the same step if sessions remain.
4. `hedge_spec()` returns delta-to-zero daily hedging (same shape the sp100 driver uses: `HedgeSpec{DELTA_TO_ZERO, DAILY, band}` with band 0.0).
5. NaN/absent strike resolution on a step (surface can't serve 40Δ): keep the previous strikes for that step (no reopen churn), never fabricate; count via a strategy-local counter exposed in Task 3's signals.

- [ ] **Step 1: Write failing tests.** Test file skeleton uses the same synthetic per-day snapshot archive machinery `backtest_swap_test.cpp` uses (copy its fixture pattern — surface per session, `Clock`, `run_backtest` with the strategy). Tests:
  - `StrangleVarswap.OpensFortyDeltaStrangleAtSnappedFixedExpiry` — after step 1: two option lots, call delta ≈ +0.40 and put delta ≈ −0.40 (tolerance 0.02 on the fixture surface), both expiries equal and == expected snapped session; qty == contracts.
  - `StrangleVarswap.RestrikesDailyAtFixedExpiry` — run 3 steps with a drifting spot fixture; each step the lot strikes change to re-hit 40Δ while every lot's expiry stays the cycle expiry; old lots are closed (book holds exactly 2 option lots each step).
  - `StrangleVarswap.RollsIntoNewCycleAtExpiry` — short tenor fixture: after the expiry session, a new pair exists with a LATER fixed expiry.
  - `StrangleVarswap.HedgeSpecIsDeltaToZeroDaily` — direct call assert.
  - `StrangleVarswap.KeepsStrikesWhenSurfaceCannotServeDelta` — fixture step with an unusable surface: strikes unchanged, no crash, run continues.
- [ ] **Step 2: Run to verify RED** (`-Ctest -R StrangleVarswap` after adding to CMake lists): expect compile failure/test failures for missing class.
- [ ] **Step 3: Implement** `strangle_varswap.hpp/.cpp` to the behavior contract. Keep on_step ≤ ~70 lines by factoring strike resolution + cycle roll into private helpers.
- [ ] **Step 4: Run to verify GREEN**: `powershell scripts\atx-build.ps1 build atx-vol-tests` then `-Ctest -R StrangleVarswap`.
- [ ] **Step 5: Commit** `feat(vol): fixed-expiry daily-restrike strangle strategy for the varswap comparison`

### Task 2: Swap leg — equal-vega var swap per cycle

**Files:**
- Modify: `atx-vol/include/atx/vol/strangle_varswap.hpp`, `atx-vol/src/strangle_varswap.cpp`
- Test: `atx-vol/tests/strangle_varswap_test.cpp` (extend)

**Interfaces:**
- Consumes: `var_swap_fair_strike` (derivatives.hpp:671-672), `deriv_greeks` (derivatives.hpp:696-699, returns `DerivGreeks{pv, delta, gamma, vega, ...}`), `SwapLot` fields (backtest.hpp:451-468), append-only swap-lot contract (backtest.hpp:400-420), option-leg entry vega from Task 1's resolved legs (`ResolvedLeg::vega`, strategy.hpp:239 — sum call+put wing vega × contracts).
- Produces: config gains `bool enable_swap_leg = true;` and `DerivConfig deriv_cfg{};`. At each cycle open, exactly one `SwapLot` appended: `kind = DerivKind::VarSwap`, `cap_dec` unset/NaN (uncapped), `strike_dec` = `var_swap_fair_strike` at the cycle's residual tenor, `expiry_ts_ns` = the SAME cycle expiry as the strangle, `n_obs_total` = number of grid sessions in (open, expiry], `annualization` = 252, `notional` = 1.0, `qty` solved so `qty * deriv_greeks(swap_contract).vega == strangle_entry_vega` (sign: long swap, long vega — both legs long vega).

Behavior contract (tested):
1. Swap opens ONCE per cycle (on the cycle-open step only, never on restrike steps).
2. Equal vega at inception: `|qty*swap_vega − strangle_vega| ≤ 1e-9 · strangle_vega`.
3. Swap expiry == strangle cycle expiry (same snapped ts).
4. `enable_swap_leg = false` reproduces Task 1 behavior bit-for-bit (A/B same-fixture run: identical option columns, all-zero `swap_pv`/`swap_pnl`).
5. If `deriv_greeks` fails or returns non-finite vega at cycle open: skip the swap for that cycle (fail-soft, counter for Task 3), never a garbage qty.

- [ ] **Step 1: Write failing tests** — `StrangleVarswap.OpensEqualVegaVarSwapAtCycleStart` (contracts 1-3 above, using an independent `deriv_greeks` call in the test as the vega oracle), `StrangleVarswap.SwapLegDisabledMatchesOptionsOnly` (contract 4), `StrangleVarswap.SkipsSwapWhenVegaUnavailable` (contract 5, unusable-surface fixture).
- [ ] **Step 2: RED run.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: GREEN run** (`-Ctest -R StrangleVarswap`).
- [ ] **Step 5: Commit** `feat(vol): open an equal-vega uncapped var swap each strangle cycle`

### Task 3: Per-step comparison signals (swap greeks + leg attribution)

**Files:**
- Modify: `atx-vol/include/atx/vol/strangle_varswap.hpp`, `atx-vol/src/strangle_varswap.cpp`
- Test: `atx-vol/tests/strangle_varswap_test.cpp` (extend)

**Interfaces:**
- Consumes: `IStrategy::signals()` contract (strategy.hpp:452-455 → `BacktestResult::signals`, backtest.hpp:1018-1020) — study how `DeclarativeStrategy`-era runs record signals and how `record_signals` is invoked in backtest.cpp before implementing; `deriv_greeks` against the step's base snapshot.
- Produces signal columns (exact names, Task 4's renderer reads them): `swap_delta`, `swap_gamma`, `swap_vega`, `swap_theta`, `swap_rho`, `strangle_vega`, `skipped_restrikes`, `skipped_swaps`. Swap greeks are qty-scaled live-lot totals recomputed each step via `deriv_greeks` with the lot's accrued `rv_spec` mirrored from the engine's accrual convention (same construction the swap lane uses: residual `maturity_t`, `rv_spec` from fixings so far — see the oracle test `backtest_swap_test.cpp` `DailySwapMarksMatchIndependentDerivPriceOracle` for the exact reference construction). NaN when no swap is live (never 0.0 — repo convention).

Behavior contract (tested):
1. On a step with a live swap, `swap_vega` equals an independent in-test `deriv_greeks` computation to 1e-12 relative.
2. Day after cycle open, `strangle_vega` reflects the restruck book (changes when strikes change).
3. Steps with no live swap: swap_* signals are NaN.
4. Signal columns are row-parallel to the result's `date` column.

- [ ] **Step 1: Write failing tests** — `StrangleVarswap.SignalsMatchIndependentSwapGreeksOracle` (contracts 1, 4), `StrangleVarswap.SignalsNaNWhenNoLiveSwap` (contract 3), extend the restrike test for contract 2.
- [ ] **Step 2: RED run.**
- [ ] **Step 3: Implement** (factor the swap-contract reconstruction into one private helper shared with Task 2's open logic — single source for the contract build).
- [ ] **Step 4: GREEN run**; also re-run the full `-R StrangleVarswap` suite.
- [ ] **Step 5: Commit** `feat(vol): emit per-step swap greeks and leg attribution signals`

### Task 4: Driver executable + comparison report renderer

**Files:**
- Create: `atx-vol/examples/strangle_varswap_driver.cpp`
- Create: `atx-vol/tools/render_strangle_vs_varswap.py`
- Test: `atx-vol/python/tests/test_render_strangle_vs_varswap.py`
- Modify: `atx-vol/CMakeLists.txt` (driver target, mirror how existing example/driver executables are declared)

**Interfaces:**
- Consumes: `SurfaceDb::open`, `Clock::from_surface_db(db).between(from,to)` (backtest.hpp:82,96), the session-probe pattern from `run_sp100_strangle_backtest.py:276-316` (translate to C++: one `load_surface(date, symbol)` per session for the snap grid), `run_backtest(clock, strategy, run_cfg)` strategy overload (backtest.hpp:1130-1131), the TSV writer the sp100 driver uses (`write_backtest_pnl_tsv` binding's C++ underlying function — locate it and call it directly), Tasks 1-3 strategy + signal column names.
- Produces: `atx-vol-strangle-varswap-driver.exe` with args `--db <root> --symbol XOM --from 2026-01-01 --to 2026-12-31 --delta 0.40 --tenor-days 91 --contracts 100 --out <dir>`; writes `<out>/track.tsv` (BacktestResult incl. `swap_pv`/`swap_pnl` and all signal columns). `render_strangle_vs_varswap.py <track.tsv> <out.html|png>` renders: panel 1 cumulative pnl overlay (strangle leg = `pnl_total`-derived option pnl + hedge + financing − swap columns vs swap leg = cumulative `swap_pnl`), panel 2 vega overlay (`gross_vega` vs `swap_vega`), panel 3 delta (`gross_delta` vs `swap_delta`), panel 4 gamma/theta overlays. Adapt `tools/tearsheet.py`'s generic TSV/panel machinery (tearsheet.py:1-60) rather than the dispersion report.

- [ ] **Step 1: Renderer TDD** — write `test_render_strangle_vs_varswap.py` against a hand-built 5-row TSV: asserts figure produced, correct series picked up, NaN swap rows tolerated. RED (`python -m pytest atx-vol/python/tests/test_render_strangle_vs_varswap.py`), implement, GREEN.
- [ ] **Step 2: Driver** — implement `strangle_varswap_driver.cpp`; keep it thin (arg parse → db open → grid probe → strategy → run → TSV). RunConfig: `unpriced = ExcludeAndReport` (hedge trading requires it), reconcile on. No unit test for the exe (no synthetic SurfaceDb builder exists C++-side); correctness rides on Tasks 1-3 suites + Task 5's real run. Build it: `powershell scripts\atx-build.ps1 build atx-vol-strangle-varswap-driver`.
- [ ] **Step 3: Commit** `feat(vol): XOM strangle-vs-varswap driver and comparison report renderer`

### Task 5: Real XOM 2026 run + docs

**Files:**
- Create: run artifacts under `C:/atx-data/backtests/xom-strangle-varswap-2026/` (out-of-repo)
- Modify: `atx-vol/README.md` (module row for `strangle_varswap.hpp`, driver usage snippet), `atx-vol/CHANGELOG.md` (entry)

- [ ] **Step 1: Verify DB coverage**: `atx-vol-surface-db.exe info --db C:/atx-data/surface-db/sp100-2026` — record the real XOM date range; set `--from/--to` to the intersection with 2026.
- [ ] **Step 2: Run the driver** for XOM 2026; capture the summary (final cumulative pnl per leg, max drawdown per leg, vega tracking error over time).
- [ ] **Step 3: Render the report**; verify panels populated for both legs.
- [ ] **Step 4: Sanity-check economics** (not a formal test, but STOP and report if violated): both legs long vega ⇒ daily pnls positively correlated; swap pnl path smoother than strangle's (no restrike churn); strangle theta bleed and swap theta same sign.
- [ ] **Step 5: Docs** — README row + CHANGELOG entry (same conventions as the vol-derivatives sprint entry).
- [ ] **Step 6: Commit** `feat(vol): document the strangle-vs-varswap comparison backtest and record the XOM 2026 run`

## Self-Review Notes

- Spec coverage: 40Δ 3M strangle (T1), delta-neutral (T1 hedge_spec), uncapped var swap 3M (T2), equal vega (T2), fixed expiry + daily restrike + projection onto every 2026 surface (T1/T5), compare pnl and greeks through time (T3 signals + T4 renderer + T5 run), XOM (config default + T5), "improvements and wiring where needed" (strategy class, signals plumbing use, driver, renderer — all additive; no engine changes required since the swap lane already supports mid-run lot addition).
- Known risk, delegated to Task 1's implementer with escalation rights: the exact `IStrategy::on_step` mutation API and signal-recording mechanics must be read from strategy.hpp/backtest.cpp rather than this plan; the file:line anchors above are verified against the current tree.
- Type consistency: `StrangleVarswapConfig`/`StrangleVsVarswapStrategy` names used identically in Tasks 1-4; signal column names fixed in Task 3 and consumed verbatim in Task 4.
