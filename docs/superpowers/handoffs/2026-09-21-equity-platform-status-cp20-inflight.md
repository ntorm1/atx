# Equity platform status — checkpoint 20 IN FLIGHT (batch 3a: earnings / options-IV families) — 2026-09-21

Worktree `C:\atx\.worktrees\equity-platform`, branch `feat/equity-platform-20260920`. Nothing committed (no commit authorized). Stopped by the user mid-checkpoint; this file is the pick-up point. cp19 is COMPLETE (see `2026-09-21-equity-platform-status-cp19.md`); cp20 is pre-registered, built, one real cell run, tests not yet green.

## Where the alpha search stands (cp14 → cp19, all NO CANDIDATE)

| checkpoint | universe | best family (pooled net h=21, t1000) | verdict |
|---|---|---|---|
| cp16/17/18 | top-1000 / top-3000 year-union | low_dollar_volume_21 +0.90 [+0.57, +1.86] | bar 1 only; illiquid-tail effect |
| cp19 | + liquidity floor (21-day mean $ADV ≥ $50M) | momentum_252 +0.47 [−0.21, +1.17] 6/7 | nothing clears bar 1; illiquidity families invert |
| cp20 (in flight) | cp19 floored universe | — | one cell run, no scorecard yet |

Cumulative declared N = 350 before cp20; cp20 declares 7 new configurations × 20 = 140 → **N = 490** for the cp20 scorecard.

## cp20 state, exactly

**Pre-registered (progress.md, "PRE-REGISTRATION cp20" + amendment R20-3):** abr_21, abr_63 (Chan–Jegadeesh–Lakonishok announcement return via `earnFlag`), earn_premium = `ts_sum(delay(earnFlag, 178), 21)` (Frazzini–Lamont expected announcer, 3-quarter lag because the 1-year lag needs 262 rows > the 256-row context warmup), iv_rv_log = `log(atmCenI_21d) − log(ts_std(ret, 21))` (Bali–Hovakimian, sign +), iv_slope = `log(atmCenI_126d / atmCenI_21d)` (sign + is a stated hypothesis), d_iv_21 = `log(atmCenI_21d / delay(atmCenI_21d, 21))`, mom252_sector_neutral = `group_neutralize(momentum_252 DSL, sector)`. Signs and DSLs are in `atx-impl/src/equity_baseline_views.hpp` (22 entries, `kEquityFamilyRetainedCount = 15`).

**Built and compiling (Debug check + Release `build-equity-rel/bin/atx-impl.exe` 20:38):**
- `equity_baseline_views.cpp`: `kOptionalFields` {market_cap, sector, earnFlag, nEarnCnt_5d, atmCenI_21d, atmCenI_126d, high, low, open} registered as borrowed spans when the context has them (`append_context_fields`); required 3 unchanged. `sector` is a DSL Group by name (typecheck `is_group_field`).
- `stage_equity_ic.cpp`: `kCheckpoint = 20`, `iteration20-` ids, `kTrialCountRule` "(22 − 15) × 5 × 2 × 2".
- Tests: fixtures in both test files gained the four fields; `stage_equity_ic_test.cpp` literals → 20.

**One real cell run** (`C:/atx/data/equity_scorecard20_ic_2014_t1000_20260921`, ledger `atx-engine/reviews/trial-ledger-cp20-batch3a.jsonl` 2 lines, receipt `-cp20-cell1`, 41 s, peak WS 279 MB, exit 0): derived warmup 256 (sector-neutral momentum = 4 + 252), vm_slots 15. Coverage (finite admitted cells of 219,220): iv_rv_log 215,818; iv_slope 215,805; d_iv_21 213,839; mom252_sector_neutral 219,220; **abr_21, abr_63, earn_premium = 0**. The three `earnFlag` families are all-NaN on real data — most likely `earnFlag` is NaN on non-announcement sessions (passthrough from the ORATS source), so `earnFlag + delay(earnFlag, 1) + …` is NaN everywhere. Not yet confirmed: no python reader for the APNLv1 context payload exists; the locator that was mapping the byte layout was stopped.

**Failing test (1/60):** `EquityBaselineViews.FamilyProgramCompilesDerivesWarmupAndGatesOnBaselineMask` line 298 expects warmup 252; the derived value is now 256 (correct). The fix — change the expectation to 256U with the comment "mom252_sector_neutral: ts_mean(.., 5) over delay(close, 252) = 256" — was prepared but NOT applied (user stopped the edit). Everything else passes (59/60 + 1 pre-existing skip).

**Not done:** the 12 remaining cells; scorecard (`--declared-n 490`); progress.md results; rulings beyond R20-1..R20-3.

## Blockers and traps for the next agent

- **earnFlag NaN encoding.** Before re-running, establish how `earnFlag` is stored (NaN vs 0 on non-event days). Two ways: (a) read APNLv1 layout from `atx-impl/src/panel_artifact.cpp` and count values in python; (b) cheaper: replace `earnFlag` in the three DSLs by a NaN-safe form — e.g. `ts_count_nans`-based or `earnFlag > 0` if a comparison op exists (registry has 74 ops: check for `gt`/`if_else`/`ts_backfill`). If the encoding is NaN-for-absent, `ts_backfill` or `max(earnFlag, 0)` won't help (NaN propagates through max in most VMs); a `nan_to_zero`-style op may be needed — one registry line. This is an amendment (R20-x) that must be logged BEFORE the sweep; the one cell already run stays on disk as attempt 1 (its 2 ledger lines count toward the record, not toward N a second time — the configurations are the same 7).
- **atx-db is write-locked** by an external process (PID 2780, `warehouse_activate.py` family; the user's `close_companyfacts_archive7_stop.py` was open in the IDE at stop time). Ruling R20-1: this session never touched atx-db. Fundamentals (batch 3b) wait for that load; when the lock clears, the data-lane plan in `2026-09-21-next-goal-prompt-v8.md` §1 applies (scope to the ~900 floored issuers; standardization ladder is global).
- Runner: `--attempt N` suffixes the reused context dir; use a new `--ic-name-prefix` for re-runs. `--atx-impl` must be absolute. Per-phase logs open in `x` mode (`--log-tag`).
- The IC stage takes the universe from the baseline recipe (`liquidity_floor`); never pass floor flags to `equity-ic`.

## Files touched this session (all uncommitted)

`atx-impl/src/{equity_baseline_views.hpp,equity_baseline_views.cpp,stage_equity_ic.cpp,stage_equity_baseline.cpp,config.hpp,config.cpp}`, `atx-engine/{include/atx/engine/eval,src/eval}/cross_section_ic.{hpp,cpp}`, `atx-impl/tests/{equity_baseline_views_test.cpp,stage_equity_ic_test.cpp}`, `CMakePresets.json` (equity-rel), `build-equity/audits/{iteration16_run_cells.py,iteration16_equity_scorecard.py}`, handoffs cp17/cp18/cp19/cp20-inflight + goal prompts v6/v7/v8/v9, `atx-engine/reviews/2026-09-21-equity-alpha-scorecard-cp1{7,8,9}.md`, ledgers `trial-ledger-cp1{7,8,9}*.jsonl`, `trial-ledger-cp20-batch3a.jsonl`, `.superpowers/sdd/equity-platform-parent-goal/progress.md`.

## Rulings this session (R17-1 … R20-3, full text in progress.md)

R17-1..8 (families on the baseline mask; derived warmup; Release + identity; parallel cells; fsum Sharpe; no post-hoc sign flips; amihud not a candidate) · R18-1..6 (LTR skipped; aux ADV hook; engine CIs stream-order dependent; mirrors not exact negations; illiquidity = one finding; DSR variance stated not tuned) · R19-1..6 (floor is a predicate; fundamentals deferred; hole propagation; attempt-1 unfloored run counts +20; cut dimension collapsed; price/volume families exhausted) · R20-1 (no atx-db while locked) · R20-2 (optional fields exposed) · R20-3 (earn_premium 3-quarter lag).
