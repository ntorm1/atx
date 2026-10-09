# Backtest Production Engine + Track Lakehouse Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the correctness/realism gaps that keep the atx-vol backtest engine research-grade, and build a lakehouse-format persistence layer (Parquet tracks + SQLite catalog + DuckDB/Python reads) that caches precomputed backtest tracks by content-addressed identity so iterative strategy development reuses instead of recomputes.

**Architecture:** Three legs. (1) *Engine correctness gate*: fix the four major accounting/lifecycle defects found in the 2026-08-05 deep review, then execution realism (quote-side fills, margin, exercise) and statistical rigor (PSR/DSR, trial registry, PBO). (2) *Scale*: cross-variant compute reuse — shared snapshot pool, mark-memo repair, sweep driver with variant-level parallelism. (3) *Lakehouse*: keep `BacktestDb` as the engine-native checkpoint/resume store, layer a Parquet + SQLite catalog on top with `track_key = SHA256(canonical_config ‖ engine_version ‖ data_snapshot_id)` as the cache identity, and a DuckDB read path from `atxpy`.

**Tech Stack:** C++20 (clang-cl, `/W4 /permissive- /WX`), GoogleTest, Apache Arrow C++ / Parquet (vcpkg), SQLite3 (WAL), DuckDB + pyarrow on the Python side (atxpy), existing RunArchive/ATXVSA2 binary containers.

## Global Constraints

- Tier-A public headers are frozen for 1.x (`kTierA` manifest, `vol_umbrella_test.cpp`); nothing in this sprint may change a Tier-A signature. New public surface enters at Tier-B or `research/`.
- Determinism invariants I1–I8 (docs/superpowers/specs/2026-07-21-atx-vol-backtest-review.md §6) must hold: bit-identical results across `n_threads`, prefetch depth, and process boundaries; golden NAV pin `247.4065016443293` on the 82-session SPY corpus may only change together with an economics-version bump (Task D1 makes this mechanical).
- `BacktestDb` v1 partitions must stay byte- and reader-compatible; new columns/lanes arrive as new schema salts + new generations, never in-place edits.
- Correctness gates run on `dev`/`rel` presets; perf claims only from `rel-avx2` on a quiet host with the P-core lease protocol (`scripts/atx-build.ps1` NOTES).
- Build/test invocation: `pwsh scripts/atx-build.ps1 build atx-vol-tests` then `build\bin\atx-vol-tests.exe --gtest_filter=<Filter>`, or `pwsh scripts/atx-build.ps1 -Ctest -R <regex>`. Bench: configure `-Bench`, run on `rel-avx2`.
- Arrow/SQLite are new third-party deps: gate behind CMake option `ATX_VOL_LAKEHOUSE` (default ON for dev, OFF for the minimal install) so the core library never links them.
- All file writes in the lakehouse follow the house discipline already in `detail/archive_util.hpp`: unique temp → flush → atomic rename; readers validate before trusting.

---

## Part I — Review synthesis (what this sprint is fixing)

### Correctness findings (2026-08-05 deep review)

| Sev | Where | Defect |
|---|---|---|
| MAJOR | `src/backtest.cpp:966-982,1039` | Swap realized leg computes `annualization·Σr²/n_done` with one fixing per **clock step** regardless of elapsed sessions; a clock coarser than the daily fixing schedule misstates realized variance (weekly steps ⇒ ~5× overstatement) with no error or count. |
| MAJOR | `src/strategy.cpp:1397-1439` | `RollAtHorizon` skips the horizon close on a no-trade step (`d.clear` only applied when a fresh cohort commits); on a skip-day at the horizon the old cohort silently rides — to expiry if entries stay unbuildable. |
| MAJOR | `src/backtest.cpp:2794-2818`, `backtest.hpp:796-813` | Default `book_entry_fill_slippage=false` books cash at `qty·mult·entry_price` while NAV marks from the model mid — fills away from mid have their cost deleted from NAV; the detector (`reconcile_nav`) is also default-off. |
| MAJOR | `src/backtest.cpp:1294-1366` | Under `ExcludeAndReport`, a deferred expiry settles at intrinsic against the FIRST later board's spot (post-expiry drift enters settlement), and when settlement spot is observed but the base board is absent, intrinsic enters cash with a zero NAV-explain — permanent NAV≠liquidation divergence, reported only as a count. |
| MINOR | `src/backtest.cpp:3106-3119` | Premium financing rate = archive-directory-order-first surface's `r` — an arbitrary constituent on multi-name snapshots. |
| MINOR | `src/backtest.cpp:3175` | Hedge share dividends default to `q_eff_at(0.25)` proxy — carry bias on high-yield names. |
| MINOR | `src/backtest.cpp:1679-1695` | `Clock::from_manifest` silently drops non-Ok fit dates; the run spans gaps as one long step with no exclusion count (fit-survivorship). |
| MINOR | `src/backtest.cpp:2780-2813` | `VolTicks` friction silently charges 0 when the entry-risk lane is not Ok under `ExcludeAndReport`. |
| MINOR | `src/backtest.cpp:2716-2735` | `record_signals` freezes the signal-name set at the first recorded row; new names later are silently dropped. |
| MINOR | `src/backtest_template.cpp:196-213` | Exact-timestamp settlement matching breaks when corpus snapshot times-of-day vary across sessions. |
| — | (positive) | **No lookahead found.** Every decision input reads the base snapshot only; PIT universe validation rejects future-effective compositions. Preserve under test. |

Persistence layer (see also Part I §3): reuse predicates omit producer code version (`surface_db.hpp:295-383` — the wrong-`--r` incident destroyed 95 surfaces with `verify` green); `backtest_series_identity` folds no engine version — invalidation rests on three hand-bumped salts (`backtest_db.cpp:1412-1433`); `surface_db_populate` partition rewrite is destructive with no versioning; cross-process single-writer entirely unenforced (no lock file / CAS; last-rename-wins manifest clobber, `backtest_db.cpp:921-941`); `vacuum_unindexed_partitions` can delete files live readers still reference.

### Performance findings

| Sev | Where | Issue |
|---|---|---|
| HIGH | `src/backtest.cpp:3287-3289` (+`:2993-2995`) | `StepMarkMemo` populates only inside `book_greeks(..., &mark_memo)`, which is skipped whenever `execute()` supplied `ex->book_greeks` — i.e. every step of a daily-hedge strategy. Memo never repopulates after inception; expiring lots re-solve, re-adding the ~1 s/u/expiry-day the L2 sprint removed. |
| HIGH | architecture | Zero cross-variant reuse: each `run_backtest` builds a private snapshot cache and re-prices near-identical `(uid,K,T,side,date)` marks. 10k variants × 5y ≈ 8–10 CPU-days at 15–19 steps/s. Only cross-variant amortization closes this (Phase C + D). |
| MED | `src/snapshot_cache.cpp:84-94` | One fresh OS thread per snapshot load (`std::async`); millions of spawns at sweep scale. |
| MED | `src/backtest.cpp:2763-2770` | `execute()` requests `FullGreeks` every step though the hedge consumes delta only; K4 first-order tier unusable while pnl-base reuse is coupled to bundle width. |
| MED | `src/pricing_executor.cpp:52-61` | ~124 µs per pool dispatch; the one global pool is the wrong arbiter for 10k concurrent single-book runs — invert to variant-level parallelism with `n_threads=1` inner. |

Current ratified numbers: `universe_strangle_hedged` ~15.2 steps/s (threads:1, provisional), post-WS-L 18.9 provisional; SPY dispersion real ~123 steps/s; solve floor 6 s/u on expiry days.

### Feature gaps vs the production checklist (literature review 2026-08-05)

From Bailey & López de Prado (PSR/DSR/MinTRL/PBO-CSCV), Harvey-Liu-Zhu haircuts, ORATS fill conventions (mid + f·half-spread, f≈0.75 single-leg / ≈0.53 four-leg), Cboe BXM/PUT roll methodology, Bakshi-Kapadia VRP framing, Nautilus/LEAN determinism-and-caching architecture:

- No quote-side fills (recorded `raw_bid/raw_ask` never used as fills) — the #1 realism gap; headline NAVs overstate capturable PnL by construction.
- No margin/capital model: cash goes arbitrarily negative, returns are on notional not required capital.
- No early-exercise/assignment simulation; no discrete-dividend share-ledger integration by default; no corporate actions.
- No trial registry → DSR/PBO uncomputable; tearsheet stops at Sharpe/drawdown/hit-rate.
- No walk-forward/CV/sweep harness; no PIT universe removals (survivorship at reconstitution).
- Persistence: three bespoke C++-only binary formats; no Python/SQL access; no compression; no cross-DB catalog ("all strategies ever computed" is unanswerable); no date-slice reads; track extension = full re-serialize.

### Storage decision record

Evaluated (2026-08-05 research): Parquet/Arrow C++, Feather, Lance (no C++ writer), DuckDB native, SQLite+blobs, Iceberg-cpp (writes not production-ready), delta-kernel-rs FFI (unstable direction), DuckLake v1.0 (viable, young), ArcticDB (right semantics, Python-only write path in practice).

**Chosen: Stack A — Parquet tracks (Arrow C++) + SQLite catalog (WAL) + DuckDB/pyarrow reads.** Rationale: only stack where the C++ engine writes natively on Windows today, multi-process-safe via SQLite WAL, full SQL analytics from Python, zero server processes, everything readable by any tool if we eject. DuckLake is the documented grow-up path (same Parquet files, catalog migrates); re-evaluate at v1.x maturity. `BacktestDb` stays: it is the engine-native resume/checkpoint store and its immutable generation-named partitions are correct; the lakehouse is the *analysis and cache* face, fed by export.

```
<lake_root>/
  tracks/underlier=SPY/family=strangle_hedged/batch-000042.parquet   # many tracks per file
  staging/<track_key>.feather                                        # fresh tracks, pre-compaction
  catalog.sqlite                                                     # tracks + trials + lineage (WAL)
```

Parquet layout: hive partitioning on low-cardinality dims only (`underlier`, `family`); `track_key` is a column (+ row-group sort key), NOT a partition; target 256–512 MB files, one row group per track batch so a track's rows stay contiguous; zstd.

---

## Part II — Implementation plan

Phase order is dependency order: A (engine correctness) and D1–D3 (identity + store) are independent and can run in parallel lanes; C needs A merged; D4–D6 need D2–D3; B4–B5 need D3 (trial registry lives in the catalog); E is last.

---

### Task A1: Swap fixing-cadence guard

**Files:**
- Modify: `atx-vol/src/backtest.cpp:966-982,1039` (realized-leg accrual), `atx-vol/include/atx/vol/backtest.hpp` (RunConfig + error vocabulary)
- Test: `atx-vol/tests/backtest_test.cpp`

**Interfaces:**
- Produces: `RunConfig::swap_fixing_cadence` (enum `SwapFixingCadence { RequireEverySession, AcceptClockAsSchedule }`, default `RequireEverySession`); new error `Error::SwapFixingScheduleViolation`.

- [ ] **Step 1: Write the failing test** — a seeded swap lot with `n_obs_total` sized for daily fixings, run on a clock with a 3-session gap, must refuse:

```cpp
TEST(BacktestSwap, CoarseClockRefusedUnderRequireEverySession) {
  auto fx = SwapBacktestFixture::daily_contract_with_gapped_clock(/*gap_sessions=*/3);
  RunConfig cfg = fx.base_config();  // swap_fixing_cadence defaults to RequireEverySession
  auto r = run_backtest(fx.corpus(), fx.strategy(), cfg);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, Error::SwapFixingScheduleViolation);
  // and the error names the offending step date + expected vs. seen fixing count
  EXPECT_THAT(r.error().message, HasSubstr("expected 1 session"));
}
TEST(BacktestSwap, GapAcceptedUnderAcceptClockAsSchedule_ScalesAccrual) {
  // opt-in mode: n_done advances by ELAPSED SESSIONS, not by 1, so
  // annualization * sum(r^2) / n_done keeps the daily-strike convention honest
  auto fx = SwapBacktestFixture::daily_contract_with_gapped_clock(3);
  RunConfig cfg = fx.base_config();
  cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;
  auto r = run_backtest(fx.corpus(), fx.strategy(), cfg);
  ASSERT_TRUE(r.ok());
  EXPECT_NEAR(fx.expected_realized_var_daily_convention(), r->swap_realized_var_at_settle(), 1e-12);
}
```

- [ ] **Step 2: Run to verify both fail** — `build\bin\atx-vol-tests.exe --gtest_filter=BacktestSwap.CoarseClock*:BacktestSwap.GapAccepted*` → FAIL (no such config field / wrong accrual).
- [ ] **Step 3: Implement** — in the fixing pass (`backtest.cpp:966`), compute `elapsed_sessions = nyse_sessions_between(base.ts_ns, shifted.ts_ns)`; under `RequireEverySession`, `elapsed_sessions != 1` for a live swap lot → fail-closed error naming the step; under `AcceptClockAsSchedule`, book the squared return once but advance `n_done += elapsed_sessions` (and document that this treats the gap return as one fixing over N slots — the conservative daily-convention read).
- [ ] **Step 4: Run gate** — filter above → PASS; then full swap suite `--gtest_filter=*Swap*` → no regressions.
- [ ] **Step 5: Commit** — `fix(backtest)!: refuse swap fixings on a clock coarser than the schedule`

---

### Task A2: RollAtHorizon closes on no-trade steps

**Files:**
- Modify: `atx-vol/src/strategy.cpp:1397-1439`
- Test: `atx-vol/tests/backtest_test.cpp`

- [ ] **Step 1: Failing test** — cohort at its roll horizon on a step where entry resolution fails (e.g. delta target unresolvable) must still close the old cohort:

```cpp
TEST(DeclarativeStrategy, RollAtHorizonClosesEvenWhenEntryUnbuildable) {
  auto fx = StrategyFixture::roll_at_horizon(/*horizon_dte=*/7);
  fx.poison_entry_resolution_on(fx.horizon_date());  // strike solver returns NotFound that day
  auto r = run_backtest(fx.corpus(), fx.strategy(), fx.config());
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(0u, fx.lots_alive_after(fx.horizon_date(), *r));   // old cohort closed
  EXPECT_GT(r->n_steps_entry_skipped, 0u);                     // and the skip is counted
}
```

- [ ] **Step 2: Verify FAIL** — old cohort still alive after horizon date.
- [ ] **Step 3: Implement** — in the no-trade branch, commit `d.clear` for `RollAtHorizon` exactly as `CloseAtHorizon` does (close is unconditional at horizon; only the re-entry is conditional). Add `n_steps_entry_skipped` counter to `BacktestResult` scalars (not the frozen 25-col wire set — a result scalar).
- [ ] **Step 4: PASS + `--gtest_filter=DeclarativeStrategy.*` clean.**
- [ ] **Step 5: Commit** — `fix(strategy): RollAtHorizon closes the aged cohort on no-trade steps`

---

### Task A3: Entry-fill slippage on by default + NAV reconciliation as a gate

**Files:**
- Modify: `atx-vol/include/atx/vol/backtest.hpp:796-813` (defaults), `atx-vol/src/backtest.cpp:2794-2818`
- Test: `atx-vol/tests/backtest_test.cpp`, golden-replay pins

- [ ] **Step 1: Failing test** — a strategy filling 5 bps away from model mid must show that cost in NAV under *default* config:

```cpp
TEST(BacktestAccounting, EntryAwayFromMidHitsNavByDefault) {
  auto fx = AccountingFixture::single_lot_entry_at(/*bps_from_mid=*/5.0);
  RunConfig cfg;                       // DEFAULTS — the point of the test
  auto r = run_backtest(fx.corpus(), fx.strategy(), cfg);
  ASSERT_TRUE(r.ok());
  EXPECT_NEAR(fx.expected_nav_with_slippage(), r->nav.back(), 1e-9);
}
TEST(BacktestAccounting, ReconcileNavIsOnByDefault) {
  RunConfig cfg;
  EXPECT_TRUE(cfg.reconcile_nav);
}
```

- [ ] **Step 2: Verify FAIL** (slippage deleted from NAV; reconcile off).
- [ ] **Step 3: Implement** — flip `book_entry_fill_slippage` default to `true`; flip `reconcile_nav` default to `true` with the existing per-row liquidation recompute; keep both as opt-outs for the bit-exact replay suites. Re-pin goldens that legitimately change (all current CLIs fill at model mid, so the 82-session golden NAV should NOT move — assert that explicitly before re-pinning anything).
- [ ] **Step 4: PASS + full `--gtest_filter=Backtest*` + golden replay suite.**
- [ ] **Step 5: Commit** — `fix(backtest)!: default-on entry-fill slippage accounting and NAV reconciliation`

---

### Task A4: Deferred expiry settles on expiry-date economics

**Files:**
- Modify: `atx-vol/src/backtest.cpp:1294-1366`
- Test: `atx-vol/tests/backtest_test.cpp:3413` region (existing pins change deliberately)

- [ ] **Step 1: Failing test** — expiry date observable in the corpus but lot's board missing: settlement intrinsic must use the **expiry date's** spot (recorded at deferral time), and when the expiry spot itself was never observed, the lot must not silently settle at a later spot:

```cpp
TEST(BacktestSettlement, DeferredSettlementUsesExpirySpotNotFirstLaterBoard) {
  auto fx = SettlementFixture::missing_board_on_expiry();  // spot observable, board absent
  auto cfg = fx.config_with(UnpricedLotPolicy::ExcludeAndReport);
  auto r = run_backtest(fx.corpus(), fx.strategy(), cfg);
  ASSERT_TRUE(r.ok());
  EXPECT_NEAR(fx.intrinsic_at_expiry_spot(), r->settlement_cash_for(fx.lot_id()), 1e-12);
}
TEST(BacktestSettlement, UnobservableExpirySpotIsCountedNotDrifted) {
  auto fx = SettlementFixture::expiry_session_entirely_absent();
  auto cfg = fx.config_with(UnpricedLotPolicy::ExcludeAndReport);
  auto r = run_backtest(fx.corpus(), fx.strategy(), cfg);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(1u, r->n_settlements_at_stale_spot);   // new counter; economics substitution is named
}
```

- [ ] **Step 2: Verify FAIL.**
- [ ] **Step 3: Implement** — `DeferredSettlementBook` records the expiry-date spot (or its absence) at deferral time; settlement cash uses that recorded spot; the truly-unobservable case books at the last pre-expiry spot **and** increments `n_settlements_at_stale_spot` plus per-lot report rows. NAV-explain gets the settlement flow it previously zeroed (fixes the permanent NAV≠liquidation divergence).
- [ ] **Step 4: PASS; update the deliberate pin at `backtest_test.cpp:3413` with a comment stating the economics change.**
- [ ] **Step 5: Commit** — `fix(backtest)!: deferred expiry settlement uses expiry-date spot; stale-spot substitutions counted`

---

### Task A5: Fail-closed hedge spot + explicit financing reference

**Files:**
- Modify: `atx-vol/src/backtest.cpp:3106-3119` (financing rate), hedge `spot_of` path, `atx-vol/include/atx/vol/backtest.hpp` (`FinancingConfig`)
- Test: `atx-vol/tests/backtest_test.cpp`

- [ ] **Step 1: Failing tests** — (a) hedge on a missing-surface step must not trade at spot 0.0 (skip + count under `ExcludeAndReport`, error under `Error`); (b) financing rate must come from `FinancingConfig::reference_uid` (or explicit flat rate), never archive order:

```cpp
TEST(BacktestHedge, MissingSurfaceHedgeIsSkippedAndCounted) {
  auto fx = HedgeFixture::missing_surface_on_hedge_day();
  auto r = run_backtest(fx.corpus(), fx.strategy(), fx.config_with(UnpricedLotPolicy::ExcludeAndReport));
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(0.0, fx.hedge_shares_traded_on(fx.gap_date(), *r));  // no free flatten at spot 0
  EXPECT_GE(r->n_hedge_steps_skipped, 1u);
}
TEST(BacktestFinancing, RateComesFromConfiguredReference) {
  auto fx = FinancingFixture::two_names_with_different_r();
  auto cfg = fx.config();  cfg.financing.reference_uid = fx.uid("SPY");
  auto r = run_backtest(fx.corpus(), fx.strategy(), cfg);
  EXPECT_NEAR(fx.expected_carry_at_spy_rate(), r->financing_cash.back(), 1e-10);
}
```

- [ ] **Step 2: FAIL → Step 3: Implement** — `spot_of` returns `Result`; hedge path branches on policy; `FinancingConfig` gains `reference_uid` (0 ⇒ require single-name corpus) or `flat_r` override; default keeps single-name behaviour bit-identical.
- [ ] **Step 4: PASS + goldens byte-stable on single-name corpora.**
- [ ] **Step 5: Commit** — `fix(backtest): fail-closed hedge spot; financing rate from explicit reference`

---

### Task A6: Clock gap accounting

**Files:**
- Modify: `atx-vol/src/backtest.cpp:1679-1695`, `BacktestResult` scalars
- Test: `atx-vol/tests/backtest_test.cpp`

- [ ] **Step 1: Failing test** — manifest with 2 non-Ok fit dates ⇒ `r->n_clock_dates_dropped == 2` and the dropped dates listed in run diagnostics; strict mode (`RunConfig::clock_gaps = Error`) refuses.
- [ ] **Step 2: FAIL → Step 3: Implement** — count + retain dropped dates in `Clock`; surface in `BacktestResult` + `run_diagnostics`; add `ClockGapPolicy { Accept, Error }` default `Accept` (behaviour-preserving).
- [ ] **Step 4: PASS.**
- [ ] **Step 5: Commit** — `feat(backtest): clock gap counters and strict gap policy`

---

### Task B1: Quote-side fill model

**Files:**
- Modify: `atx-vol/include/atx/vol/backtest.hpp` (`FrictionModel`), `atx-vol/src/backtest.cpp:2741-2900` (`execute()`)
- Test: `atx-vol/tests/backtest_exec_test.cpp`

**Interfaces:**
- Produces: `SpreadKind::QuoteSide` — fill = mid + `f(leg_count)`·half-spread from **recorded** `raw_bid/raw_ask` when present, model-spread fallback; `FrictionModel::crossing_fraction_single = 0.75`, `crossing_fraction_complex = 0.53` (ORATS calibration, overridable); every `BacktestResult` carries `friction_regime` (enum: `Frictionless|Modeled|QuoteSide`) so no artifact leaves the engine without its assumption attached.

- [ ] **Step 1: Failing tests** — (a) with recorded quotes, a 2-leg entry fills at `mid ± f·(ask-bid)/2` per side with `f = crossing_fraction_complex`; (b) absent quotes fall back to the modeled `PriceBps` half-spread; (c) `friction_regime` is stamped:

```cpp
TEST(BacktestExec, QuoteSideFillCrossesRecordedSpread) {
  auto fx = ExecFixture::listed_quotes(/*bid=*/1.00, /*ask=*/1.10);
  auto cfg = fx.config(); cfg.friction.kind = SpreadKind::QuoteSide;
  auto r = run_backtest(fx.corpus(), fx.buy_one_lot_strategy(), cfg);
  // buy fills at 1.05 + 0.75*0.05 = 1.0875 (single leg)
  EXPECT_NEAR(1.0875, fx.entry_fill_price(*r), 1e-12);
  EXPECT_EQ(FrictionRegime::QuoteSide, r->friction_regime);
}
```

- [ ] **Step 2: FAIL → Step 3: Implement** — thread `raw_bid/raw_ask` (already recorded on the listed route) into `execute()`'s entry/roll/close fills; direction-aware sign; leg-count from the committed cohort's structure; hedge shares keep their own `hedge_slippage_bps`. NAV impact flows through A3's now-default slippage accounting.
- [ ] **Step 4: PASS + frictionless replay (`SpreadKind::None`) still bit-identical (invariant I3).**
- [ ] **Step 5: Commit** — `feat(backtest): quote-side fills with per-leg-count crossing fractions`

---

### Task B2: Margin engine + return-on-capital

**Files:**
- Create: `atx-vol/include/atx/vol/margin.hpp`, `atx-vol/src/margin.cpp`
- Modify: `atx-vol/src/backtest.cpp` (per-step margin column), `atx-vol/src/tearsheet.cpp`
- Test: `atx-vol/tests/margin_test.cpp` (new)

**Interfaces:**
- Produces: `MarginModel { RegT, ScenarioGrid }`; `regt_short_option_margin(spot, strike, premium, mult, Side) -> double` implementing `max(0.20·S − OTM, 0.10·S)·mult + premium` (puts: `0.10·K` floor); `scenario_margin(const PortfolioPricer&, const MarginScenarioSpec&) -> Result<double>` = worst loss on the existing `scenario_grid.hpp` (spot ±15%, vol ±) revalued book; per-step `margin_required` series; tearsheet `return_on_margin`, `margin_utilization_peak`.

- [ ] **Step 1: Failing unit tests** — RegT formula against hand-computed cases (OTM put, ITM call, deep-OTM floor case); scenario margin equals the known worst cell of a pinned 2-lot book on the flat-surface fixture.
- [ ] **Step 2: FAIL → Step 3: Implement** — pure functions first (no engine wiring), `scenario_margin` reuses `scenario_grid`'s Taylor+exact-repricing machinery with the PM shift spec.
- [ ] **Step 4: PASS → Step 5: Commit** — `feat(vol): Reg-T and scenario-grid margin models`
- [ ] **Step 6: Engine wiring test** — `margin_required` recorded per step; `RunConfig::margin_breach = {Ignore, Halt}`; breach halts with a named event row. FAIL → implement → PASS.
- [ ] **Step 7: Commit** — `feat(backtest): per-step margin requirement and breach policy`

---

### Task B3: Early exercise / assignment checks

**Files:**
- Modify: `atx-vol/src/backtest.cpp` (`compute_step` pre-settlement pass), `atx-vol/include/atx/vol/backtest.hpp`
- Test: `atx-vol/tests/backtest_test.cpp`

**Interfaces:**
- Produces: `ExercisePolicy { Advisory, Simulate }` (default `Advisory` — behaviour-preserving). Advisory: per-step counters `n_short_calls_assignable` (short call, ex-div next session, extension value < forward dividend) and `n_puts_exercisable` (deep-ITM put, extension < carry). Simulate: assignment converts the option lot to the share ledger at intrinsic on the ex-div-preceding close, with the A5 financing machinery carrying the resulting shares.

- [ ] **Step 1: Failing tests** — pinned fixture: short ITM call over a $1 discrete dividend with 2 DTE ⇒ advisory counter fires; under `Simulate`, the lot converts to short shares + cash at intrinsic and NAV thereafter tracks the share ledger:

```cpp
TEST(BacktestExercise, ShortItmCallOverExDivFlaggedAdvisory) { /* counter == 1 on ex-div-1 */ }
TEST(BacktestExercise, SimulatePolicyConvertsToShareLedger) { /* lot gone, shares == -qty*mult, cash += intrinsic-side flow */ }
```

- [ ] **Step 2: FAIL → Step 3: Implement** — the check needs only data already on the step: discrete dividend schedule (A5), per-lot mark and intrinsic, `q_eff`/r for carry. Keep the decision rule a pure function `should_exercise_early(...)` in the header for testability.
- [ ] **Step 4: PASS; default-policy goldens byte-identical.**
- [ ] **Step 5: Commit** — `feat(backtest): early exercise/assignment — advisory counters and opt-in simulation`

---

### Task B4: Rigor tearsheet — PSR / DSR / MinTRL + residual alarm

**Files:**
- Modify: `atx-vol/src/tearsheet.cpp`, `atx-vol/include/atx/vol/tools/` tearsheet header
- Test: `atx-vol/tests/tearsheet_test.cpp`

**Interfaces:**
- Consumes: trial counts from the catalog (Task D3) via a plain struct `TrialStats { uint64_t n_trials; double sr_variance; }` — the tearsheet does NOT link SQLite; the driver passes the numbers in.
- Produces: `psr(sr, skew, kurt, T, benchmark)`, `dsr(sr, skew, kurt, T, TrialStats)`, `min_trl(sr, skew, kurt, benchmark, alpha)` as pure functions (formulas: PSR = Φ[(SR−c)√(T−1)/√(1−γ₃SR+((γ₄−1)/4)SR²)]; SR₀ = √V[SR]·[(1−γ)Z⁻¹(1−1/N)+γZ⁻¹(1−1/(Ne))], γ=0.5772…; DSR = PSR(SR₀)); attribution `unexplained_alarm` when |unexplained|/gross exceeds tolerance over a rolling window.

- [ ] **Step 1: Failing tests** — pin PSR/DSR/MinTRL against hand-computed reference values (e.g. SR=1.0, skew=−1, kurt=5, T=252, N=100 trials → DSR value computed offline with scipy, embedded as the expected constant); residual alarm trips on a fixture with a deliberately mis-marked greek.
- [ ] **Step 2: FAIL → Step 3: Implement (pure functions; `ats_norm_cdf` already in-tree) → Step 4: PASS.**
- [ ] **Step 5: Commit** — `feat(tearsheet): probabilistic and deflated Sharpe, MinTRL, attribution residual alarm`

---

### Task B5: PBO/CSCV harness (Python, reads the lakehouse)

**Files:**
- Create: `python/src/atxpy/pbo.py`, `python/tests/test_pbo.py`

**Interfaces:**
- Consumes: track matrix from Task D4's `atxpy.tracks.load(...)` (T×N daily returns DataFrame).
- Produces: `cscv_pbo(returns: pd.DataFrame, n_blocks: int = 16) -> PboResult(pbo, degradation_slope, p_oos_loss, lambdas)` — S row-blocks, all C(S, S/2) train/test splits, IS-winner OOS relative rank ω, λ=ln(ω/(1−ω)), PBO = fraction λ≤0.

- [ ] **Step 1: Failing tests** — (a) pure-noise matrix (seeded RNG, 16 blocks, 20 configs) ⇒ PBO ≈ 0.5 (assert ∈ [0.35, 0.65]); (b) one genuinely dominant config (mean-shifted) ⇒ PBO < 0.1; (c) λ count == C(16,8).
- [ ] **Step 2: FAIL → Step 3: Implement with numpy/itertools → Step 4: `pytest python/tests/test_pbo.py -v` PASS.**
- [ ] **Step 5: Commit** — `feat(atxpy): CSCV probability-of-backtest-overfitting harness`

---

### Task C1: Mark-memo repopulation on the execute() path

**Files:**
- Modify: `atx-vol/src/backtest.cpp:2763-2770,3287-3289`, `atx-vol/src/step_mark_memo.hpp`
- Test: `atx-vol/tests/backtest_exec_test.cpp` (solve-ledger gates)

- [ ] **Step 1: Failing test** — daily-hedge strategy over an expiry: assert via the `Solve` ledger that expiry-day settlement marks are memo hits, not fresh solves:

```cpp
TEST(BacktestSolveLedger, ExpiryMarksMemoHitOnDailyHedgeRoute) {
  auto fx = ExecFixture::daily_hedged_strangle_over_expiry();
  auto r = run_backtest(fx.corpus(), fx.strategy(), fx.config());
  EXPECT_EQ(0u, r->solve_ledger.settlement_full_solves);   // today: > 0 — the bug
  EXPECT_GE(r->solve_ledger.settlement_memo_hits, fx.n_expiring_lots());
}
```

- [ ] **Step 2: FAIL → Step 3: Implement** — `execute()`'s FullGreeks `price_into` pass calls `mark_memo->populate_from(bundle, base_surface_instance)` exactly as `book_greeks` does; the surface-instance guard then passes on the following expiry step. Also add a `settlement_memo_hits/full_solves` pair to the solve ledger if not present.
- [ ] **Step 4: PASS + bit-identity vs pre-fix on a non-expiry corpus (memo affects solve count, never values — assert golden NAV unchanged).**
- [ ] **Step 5: Commit** — `perf(backtest): repopulate step-mark memo from the execute() greeks pass`

---

### Task C2: Process-wide snapshot pool + variant-parallel topology

**Files:**
- Create: `atx-vol/include/atx/vol/research/snapshot_pool.hpp`, `atx-vol/src/snapshot_pool.cpp`
- Modify: `atx-vol/src/snapshot_cache.cpp:84-94` (executor-routed loads), `atx-vol/src/backtest.cpp:2936-2943` (accept shared pool)
- Test: `atx-vol/tests/backtest_exec_test.cpp`

**Interfaces:**
- Produces: `SnapshotPool` — process-wide, read-only, lock-sharded (shard by date hash) map date→Sealed mmap snapshot; `RunConfig::snapshot_pool` (non-owning pointer; null ⇒ today's private cache, bit-identical behaviour). Loads route through the persistent `PricingExecutor` I/O lane, not `std::async`.

- [ ] **Step 1: Failing tests** — (a) two sequential runs sharing a pool: second run performs zero archive opens (pool telemetry); (b) determinism: pooled vs private cache runs are `memcmp`-identical on `BacktestResult`; (c) 8 concurrent runs (std::jthread) sharing the pool: results identical to serial.
- [ ] **Step 2: FAIL → Step 3: Implement** — Sealed tiers only (immutable mmap views); identity probe once per (date, generation); eviction = explicit `trim(dates_before)` driven by the sweep driver's watermark, no LRU races.
- [ ] **Step 4: PASS on `rel`; ThreadSanitizer-equivalent soak via the 8-thread test looped 100× in CI.**
- [ ] **Step 5: Commit** — `feat(research): process-wide sealed snapshot pool with executor-routed loads`

---

### Task C3: Sweep driver — enumerate, dedupe, cache-first, variant-parallel

**Files:**
- Create: `atx-vol/include/atx/vol/research/sweep_driver.hpp`, `atx-vol/src/sweep_driver.cpp`
- Test: `atx-vol/tests/sweep_driver_test.cpp` (new)

**Interfaces:**
- Consumes: `SnapshotPool` (C2), `TrackKey` + catalog probe (D1/D3), `run_backtest` (unchanged).
- Produces: `SweepSpec { std::vector<BacktestStrategyTemplate> variants; ... }`; `run_sweep(const SweepSpec&, const SweepConfig&) -> SweepResult` — fingerprint-dedupes identical variants, probes the catalog by `track_key` (hit ⇒ skip), schedules misses on a variant-level `parallel_for` with `opts.n_threads=1` inner runs, writes each finished track to staging + catalog (D5), and registers every variant in the trial registry (B4's N).

- [ ] **Step 1: Failing tests** — (a) 4 variants where 2 are identical ⇒ 3 engine runs, 4 catalog rows (the duplicate maps to the same track_key); (b) rerun of the same sweep ⇒ 0 engine runs, all hits; (c) sweep result NAVs `memcmp`-equal to individually-run baselines.
- [ ] **Step 2: FAIL → Step 3: Implement → Step 4: PASS.**
- [ ] **Step 5: Commit** — `feat(research): cache-first variant sweep driver`

---

### Task D1: Content-addressed track identity + economics tripwire

**Files:**
- Create: `atx-vol/include/atx/vol/research/track_key.hpp`, `atx-vol/src/track_key.cpp`
- Modify: `atx-vol/src/backtest_db.cpp:1412-1433` (fold full identity), CI golden-replay job
- Test: `atx-vol/tests/track_key_test.cpp` (new)

**Interfaces:**
- Produces:

```cpp
struct TrackKey { std::array<std::uint8_t, 32> sha256; std::string hex() const; };
// canonical_config: RunArchive-serialized BacktestStrategyTemplate + RunConfig economics
//   fields (frictions, financing, policies), defaults materialized, doubles bit-patterned.
// engine_id: ATX_VOL_VERSION_STRING + kBacktestEconomicsRev + run_archive schema_hash.
// data_snapshot_id: SHA256 over the sorted per-date SurfaceDb partition content
//   identities the run consumed (already recorded as BacktestDb source lineage).
TrackKey make_track_key(std::span<const std::uint8_t> canonical_config,
                        std::string_view engine_id,
                        std::span<const std::uint8_t, 32> data_snapshot_id);
inline constexpr int kBacktestEconomicsRev = 1;  // bumped ONLY with a golden-NAV change
```

- SHA-256: reuse ResearchDb's existing implementation (`src/research_db.cpp`) — hoist it to a shared detail header rather than adding a second one.

- [ ] **Step 1: Failing tests** — (a) same config twice ⇒ identical key; (b) any single economics field change (friction f, financing rate, policy enum) ⇒ different key; (c) same config, different `kBacktestEconomicsRev` ⇒ different key; (d) key is stable across process restarts (golden hex pin — this is what `hash_bytes` could never give).
- [ ] **Step 2: FAIL → Step 3: Implement → Step 4: PASS.**
- [ ] **Step 5: Economics tripwire** — CI job: run the 82-session golden replay; if `final_nav != 247.4065016443293` AND `kBacktestEconomicsRev` unchanged vs main ⇒ fail with "economics changed without a rev bump". This converts the hand-bump-salt failure mode (persistence review HIGH) into a mechanical gate.
- [ ] **Step 6: Commit** — `feat(research): content-addressed track identity with economics-rev tripwire`

---

### Task D2: Parquet track writer + compactor

**Files:**
- Create: `atx-vol/include/atx/vol/research/track_store.hpp`, `atx-vol/src/track_store.cpp`, `atx-vol/tools/track_compact.cpp` (CLI)
- Modify: root `CMakeLists.txt` / vcpkg manifest (`arrow` with parquet+zstd features, gated on `ATX_VOL_LAKEHOUSE`)
- Test: `atx-vol/tests/track_store_test.cpp`

**Interfaces:**
- Produces: `TrackStore::write_staging(const TrackKey&, const BacktestResult&, const TrackMeta&) -> Result<void>` (one Feather file per fresh track in `staging/`, atomic rename); `compact(lake_root)` folds staging into `tracks/underlier=X/family=Y/batch-NNNNNN.parquet` (zstd, one row group per batch, rows sorted by `(track_key, date)`); schema v1 = the 25 frozen series columns + `track_key`(binary16→hex), `date`(date32), `ts_ns`(int64) + the swap lane (`swap_pv`, `swap_pnl`, `gross_vega_abs`, `nav_liquidation`, `step_pnl_total`) that the frozen TSV set could never carry — the lakehouse schema is where the wire set finally widens.
- Consumes: `TrackKey` (D1).

- [ ] **Step 1: Failing round-trip test** — write a 3-track staging set, compact, read back via Arrow C++: row counts, per-column values (spot-check 5 cells vs the source `BacktestResult`), hive path `underlier=SPY/family=strangle_hedged`, one row group per batch, zstd codec asserted from file metadata.
- [ ] **Step 2: FAIL (no such lib) → Step 3: Implement** — `arrow::Table` from SoA columns (they are already contiguous vectors — zero transform), `parquet::WriteTable` with `WriterProperties{compression=ZSTD, sorting_columns=(track_key,date)}`; compactor scans staging, groups by (underlier, family), targets 256–512 MB output files, deletes staged inputs only after the batch file's atomic rename lands.
- [ ] **Step 4: PASS on `dev` preset with `ATX_VOL_LAKEHOUSE=ON`; core build with `OFF` still links Arrow-free (CI matrix leg).**
- [ ] **Step 5: Commit** — `feat(research): parquet track store with staging + compaction`

---

### Task D3: SQLite catalog — tracks, trials, lineage, single-writer enforcement

**Files:**
- Create: `atx-vol/include/atx/vol/research/catalog.hpp`, `atx-vol/src/catalog.cpp`
- Modify: `atx-vol/src/backtest_db.cpp:921-941` (writer lock), `atx-vol/src/surface_db.cpp:1070-1104` (same)
- Test: `atx-vol/tests/catalog_test.cpp`

**Interfaces:**
- Produces: `Catalog` over `catalog.sqlite` (WAL mode):

```sql
CREATE TABLE tracks(
  track_key TEXT PRIMARY KEY,          -- hex SHA-256
  underlier TEXT NOT NULL, family TEXT NOT NULL,
  config_json TEXT NOT NULL,           -- canonical, human-queryable copy
  engine_id TEXT NOT NULL, economics_rev INTEGER NOT NULL,
  data_snapshot_id TEXT NOT NULL,
  date_min TEXT NOT NULL, date_max TEXT NOT NULL,
  status TEXT NOT NULL CHECK(status IN ('staging','compacted','retired')),
  file TEXT, row_group INTEGER,        -- NULL while staging
  created_ts INTEGER NOT NULL, last_access_ts INTEGER NOT NULL);
CREATE TABLE trials(                   -- B4's N: EVERY variant ever attempted
  trial_id INTEGER PRIMARY KEY,
  track_key TEXT NOT NULL REFERENCES tracks(track_key),
  sweep_id TEXT NOT NULL, sharpe REAL, created_ts INTEGER NOT NULL);
CREATE INDEX idx_tracks_dims ON tracks(underlier, family, date_min, date_max);
```

  API: `probe(TrackKey) -> optional<TrackRow>`; `register_staging(...)`; `mark_compacted(...)`; `record_trial(...)`; `trial_stats(sweep_id) -> TrialStats` (feeds B4's DSR).
- Also: cross-process writer lock for `BacktestDb`/`SurfaceDb` manifests — `manifest.lock` created with `CREATE_NEW` semantics (Win32 `CreateFileW` + `CREATE_NEW`, POSIX `O_EXCL`), held for the manifest read-modify-publish window; stale-lock takeover after a PID-liveness probe. Fixes the last-rename-wins clobber.

- [ ] **Step 1: Failing tests** — (a) probe-miss → register → probe-hit round trip; (b) two processes (spawn a child `atx-vol-tests.exe --catalog-writer-stress`) inserting 1000 rows each ⇒ 2000 rows, zero corruption (WAL does this; the test proves our pragmas); (c) manifest lock: second writer blocks/fails-cleanly instead of clobbering — regression for `backtest_db.cpp:921` (write from two handles, assert generation sequence is gap-free).
- [ ] **Step 2: FAIL → Step 3: Implement (sqlite3 amalgamation via vcpkg, `PRAGMA journal_mode=WAL; busy_timeout=5000; synchronous=NORMAL`) → Step 4: PASS.**
- [ ] **Step 5: Commit** — `feat(research): sqlite track catalog + trial registry; enforced single-writer manifests`

---

### Task D4: Python read path (atxpy + DuckDB)

**Files:**
- Create: `python/src/atxpy/tracks.py`, `python/tests/test_tracks.py`

**Interfaces:**
- Produces:

```python
def load(lake_root, *, underlier=None, family=None, date_range=None,
         track_keys=None, columns=None) -> pyarrow.Table:
    """DuckDB over tracks/**/*.parquet with hive_partitioning=1;
    pushes underlier/family to partition pruning, date/track_key to row-group stats."""
def catalog(lake_root) -> pandas.DataFrame       # the tracks table
def returns_matrix(lake_root, sweep_id) -> pandas.DataFrame  # T×N pivot for B5's PBO
```

- [ ] **Step 1: Failing tests** — build a 3-track lake via the D2 C++ writer (invoke the compiled test helper or a checked-in fixture lake), then: partition-pruned load touches only the matching directory (assert via DuckDB `EXPLAIN` containing the pruned path count), column projection returns only requested columns, `returns_matrix` pivots to T×N with NaN-free alignment.
- [ ] **Step 2: FAIL → Step 3: Implement (duckdb + pyarrow, read-only) → Step 4: `pytest python/tests/test_tracks.py -v` PASS.**
- [ ] **Step 5: Commit** — `feat(atxpy): duckdb/pyarrow track lake reader`

---

### Task D5: Cache-first integration (sweep ⇄ lakehouse)

**Files:**
- Modify: `atx-vol/src/sweep_driver.cpp` (C3), `atx-vol/src/track_store.cpp`
- Test: `atx-vol/tests/sweep_driver_test.cpp`

- [ ] **Step 1: Failing end-to-end test** — sweep of 6 variants on the SPY fixture corpus, cold lake: 6 runs, 6 staged tracks, 6 catalog rows, 6 trial rows. Same sweep again: **0 engine runs**, 6 probe-hits, trial rows appended (N grows — trials count attempts, not unique configs). Change `kBacktestEconomicsRev`: 6 fresh runs (old rows retired-by-supersession, both generations queryable). Then compact + reload via D4: values match the original `BacktestResult`s to the double.
- [ ] **Step 2: FAIL → Step 3: Wire probe→skip / miss→run→`write_staging`→`register_staging`→`record_trial` into `run_sweep` → Step 4: PASS.**
- [ ] **Step 5: Commit** — `feat(research): cache-first sweeps against the track lakehouse`

---

### Task D6: Retention, GC, vacuum safety

**Files:**
- Modify: `atx-vol/src/catalog.cpp`, `atx-vol/tools/track_compact.cpp`, `atx-vol/src/backtest_db.cpp` (`vacuum_unindexed_partitions`)
- Test: `atx-vol/tests/catalog_test.cpp`

- [ ] **Step 1: Failing tests** — (a) `gc(lake_root, older_than)` retires tracks by `last_access_ts`, rewrites affected Parquet batches without the retired rows, and never deletes a file while a registered reader epoch is open (epoch = catalog row `readers_epoch`, incremented by `load`-side `BEGIN`… replaced with: reader takes a shared advisory mark in SQLite, GC skips marked batches); (b) `BacktestDb::vacuum_unindexed_partitions` refuses while a `manifest.lock`-registered reader epoch from D3 is live (fixes the live-reader deletion hazard at `backtest_db.hpp:131-140`).
- [ ] **Step 2: FAIL → Step 3: Implement → Step 4: PASS.**
- [ ] **Step 5: Commit** — `feat(research): epoch-guarded GC for track lake and backtest-db vacuum`

---

### Task E1: Wire the dead config surface

**Files:**
- Modify: `atx-vol/examples/*.cpp` (mag7/SPY CLIs), `atx-vol/tools/`
- Test: example smoke tests in `atx-vol/tests/`

- [ ] **Step 1:** CLI flags for `FinancingConfig`, `hedge_slippage_bps`, `SpreadKind::QuoteSide` + crossing fractions, `UnpricedLotPolicy` (mag7 CLI moves off the truncating `ExcludeAndReport` default to `Error`), `ExercisePolicy`, margin model. Every emitted artifact now prints its friction regime + economics rev (A3/B1/D1 made these exist).
- [ ] **Step 2: Smoke test asserting the flags reach `RunConfig` (parse → dump → compare). PASS.**
- [ ] **Step 3: Commit** — `feat(tools): expose financing/friction/policy config on the backtest CLIs`

---

### Task E2: Tier promotion + docs

**Files:**
- Modify: `atx-vol/README.md`, `atx-vol/CHANGELOG.md`, tier manifest test (`vol_umbrella_test.cpp` Tier-B list)
- Create: `atx-vol/docs/backtest-lakehouse.md`

- [ ] **Step 1:** Promote `research/sweep_driver.hpp`, `research/track_key.hpp`, `research/track_store.hpp`, `research/catalog.hpp`, `research/snapshot_pool.hpp` → Tier-B (public-unfrozen; Tier-A stays untouched per global constraints). Update the machine-checked tier counts.
- [ ] **Step 2:** `docs/backtest-lakehouse.md`: lake layout, track_key recipe, invalidation story (economics rev + tripwire), DuckDB query cookbook, GC/compaction operations, the "what invalidates what" table.
- [ ] **Step 3:** CHANGELOG entries for every behavioural default flip (A3, A5, B1) with migration lines.
- [ ] **Step 4: Commit** — `docs(vol): backtest lakehouse guide + tier promotion`

---

### Task E3: Perf re-baseline + CI gates

**Files:**
- Modify: `atx-vol/bench/backtest_throughput_bench.cpp` (+ sweep bench), `bench/baselines/`
- CI config

- [ ] **Step 1:** New bench: `sweep/cached_rerun` — 32-variant sweep, warm lake ⇒ measures probe+load path (target: ≥100× the cold run; this is the sprint's headline number). Re-baseline `universe_strangle_hedged` post-C1/C2 on quiet `rel-avx2` host (expect ≥ the 18.9 provisional to ratify ≥1.26×, plus C1's expiry-day solve reduction). CV ≤ 5% for citability per the bench-lease protocol.
- [ ] **Step 2:** CI gates: determinism (n_threads 1 vs N memcmp), golden replay + economics tripwire (D1), `ATX_VOL_LAKEHOUSE=OFF` link check, 8-thread pool soak (C2).
- [ ] **Step 3: Commit** — `bench(vol): sweep cache bench + ratified backtest baselines`

---

## Definition of done

1. All Phase A tests green; golden NAV pin either byte-identical or re-pinned with a `kBacktestEconomicsRev` bump and CHANGELOG migration note — never silently.
2. A sweep re-run against a warm lake performs zero engine runs (D5 test) and the DSR in the tearsheet consumes the true trial count N from the catalog.
3. Python: `atxpy.tracks.load(lake, underlier="SPY", family="strangle_hedged")` returns a pruned Arrow table; `cscv_pbo(returns_matrix(...))` produces a PBO on a real sweep.
4. No artifact leaves the engine without `friction_regime`, `engine_id`, `economics_rev`, `data_snapshot_id` attached.
5. Two concurrent writer processes cannot corrupt any manifest (D3 stress test), and GC cannot delete data under a live reader (D6).
6. Bench: `sweep/cached_rerun` ≥100× cold; hedged-universe baseline ratified at CV ≤ 5%.

## Explicitly out of scope (this sprint)

- Intraday snapshots / session-boundary modeling (design keeps `now_ts_ns` keying so it slots in later).
- DuckLake migration (documented as the grow-up path; catalog schema kept boring precisely so it can move).
- Multi-strategy netting / capital allocation across books; corporate-action (split) adjustment engine.
- Remote/object-store tier for the lake (layout is rclone/S3-syncable by construction; no code).
- Decoupling pnl-base stamp reuse from bundle width (the K4 ~2× lever) — follow-on perf sprint; C1/C2/C3 land the order-of-magnitude win first.
