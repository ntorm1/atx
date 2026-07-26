# atx-vol Backtest Engine / Event Loop / Strategy / PnL — Review

Scope: `atx-vol/src/backtest.cpp` (1825 LOC, read full), `include/atx/vol/backtest.hpp` (read full),
`src/strategy.cpp` + `include/.../strategy.hpp` (read full), `src/scenario_grid.cpp`,
`src/pnl_attribution.cpp`, `src/tearsheet.cpp`, `src/run_report.cpp` (read full),
`src/session.cpp` (query path) + `session.hpp`. Cross-checked `PnlTotals`/`reduce_pnl_totals`
semantics in `portfolio_pricer.{hpp,cpp}` and `CMakeLists.txt`. READ-ONLY audit; no build.

**Bottom line: the forward pass is clean. NO look-ahead bias and NO PnL-accounting
CRITICAL was found.** The engine decides and fills strictly at the current snapshot and
reprices the previously-decided book onto the next snapshot; cash and MTM reconcile over a
lot's life. The material findings are (a) silent metric corruption when `record_every_n > 1`,
(b) American early-exercise/assignment not modeled, and (c) a set of realism/feature gaps.

---

## TIMESTAMP TIMELINE (proven from the code)

Let snapshots be `S0=refs[0] … Sk`, timestamps `t0 < t1 < …`, `tj = Sj.now_ts_ns` (all surfaces
in an archive must agree on `now_ts_ns`, `backtest.cpp:1047-1053`).

**Strategy overload (`run_backtest(clock, IStrategy&, cfg)`, backtest.cpp:1361-1823):**

Row 0 (inception, 1639-1680):
- `strat.on_step(base=S0, step=0)` resolves strikes-from-delta, sizing greeks, and entry marks
  ALL on `S0`'s surface @ `t0` (strategy.cpp `expand_leg` 421-539, entry_price = model mark @ t0,
  strategy.cpp:844).
- `execute(base=S0)` books entry frictions/premium and the opening hedge on `S0` @ `t0`; hedge
  shares fill at `S0` spot (`spot_of(uid)=base_snap.find(uid)->pricing().S`, 1606-1609).
- Row: `nav = -cost`, book greeks @ `t0`. **Decision @ t0, fill @ t0.**

Step i≥1 (1682-1820):
1. `compute_step(base=S_{i-1}, shifted=S_i, book.lots)` — the book as it stood after step i-1's
   `on_step`. `price_base` @ `t_{i-1}`, `price_target` @ `t_i`
   (`pnl_total_ps = price_target - price_base`, portfolio_pricer.cpp:1463). Forward MTM over
   `[t_{i-1}, t_i]` of the PREVIOUSLY decided book. `target_marks` = per-lot marks @ `t_i`.
2. `shares_pnl = Σ n·(S@t_i − S@t_{i-1})`; financing accrued over the exact ns gap `[t_{i-1},t_i]`
   (1718-1746) — carry over calendar time, spans weekends correctly.
3. `base = S_i`; expiring lots (`expiry_ts == t_i`) settle at intrinsic @ `t_i` into cash
   (1750-1763); expired lots erased.
4. `before_lots = survivors`; `strat.on_step(base=S_i, step=i)` makes NEW decisions on `S_i` @ `t_i`.
5. `execute(base=S_i)`: entries fill at model mark @ `t_i`; roll-closes fill at `target_marks`
   (marks @ `t_i`, = current base) or fallback `fair_value` on `S_i`; hedge on `S_i` @ `t_i`.
6. `step_total = pnl_total + settlement + shares_pnl + financing − cost`; `nav += step_total`.

**Marks sampled at:** base=`t_{i-1}` and shifted=`t_i` for forward MTM; `t_i` for settlement,
new entries, roll-closes, and hedge. **Orders decided at:** `t_{i-1}` (the book being MTM'd) and
`t_i` (new trades). **Fills at:** the same `t_i` the decision uses. The roll-close reuses the
`t_i` target mark computed inside `compute_step` — the mark date EQUALS the decision date after
the move-swap, so this optimization is NOT a leak.

**Fixed-book overload (1182-1350):** static book, pure MTM `S_{i-1}→S_i` each step, exact-expiry
settlement; no decisions, no cash ledger. `entry_price` is validated but never used in P&L (P&L
marks from the surface at `S0`).

### LOOK-AHEAD VERDICT: CLEAN.
Every decision and every fill occurs at the current snapshot's timestamp; forward P&L is strictly
the prior-decided book repriced onto the next snapshot. `pnl_total` is the exact reprice
(`price_target − price_base`), so `nav` is a true mark-to-market total return, not a Taylor
approximation (the 8 Greek axes + `unexplained` are attribution only). No mark/quote/surface is
ever sampled at-or-after a later timestamp than the decision it feeds.

---

## TOP 5

1. **[HIGH] `record_every_n > 1` silently corrupts every per-step-derived metric.** `nav`
   accumulates EVERY step; the `pnl_*`/attribution columns store ONLY the recorded step. Sharpe,
   ann_return, ann_vol, hit_rate, attribution totals and avg_daily_pnl are then computed off a
   1-in-`stride` sample and are wrong, with no warning. (backtest.cpp:1325/1332/1342, 1792/1796/1813;
   tearsheet.cpp:79-92,120-132; run_report.cpp:195-198)
2. **[MEDIUM] American early exercise / assignment not modeled.** Options are held and MTM'd at
   American marks then settled European-style at intrinsic; early exercise is never taken and short
   early-assignment risk is absent. (backtest.cpp compute_step 707-768; loop 1750-1763)
3. **[MEDIUM] Synthetic-tenor lots can essentially never settle.** Settlement requires an EXACT
   snapshot-timestamp match, but `DeclarativeStrategy` sets `expiry_ts_ns = base_ts +
   round(T·yr)`, which won't coincide with real close timestamps → HoldToExpiry aborts `NotFound`
   at the first expiry crossing. (backtest.cpp:707-714; strategy.cpp:50-71,834-840)
4. **[MEDIUM] Default fills are model MID with zero cost; even with frictions on, "bid/ask" is a
   synthetic model over a fitted MID surface (no real quotes).** (backtest.hpp:257-264 default
   `SpreadKind::None`; backtest.cpp execute 1532-1596)
5. **[MEDIUM] Account-level financing uses arbitrary references:** cash accrues at
   `surfaces().front().pricing().r` (first surface in archive order) and shares carry uses a
   hardcoded `q_eff_at(0.25)` tenor. (backtest.cpp:1723,1744)

---

## FINDINGS BY SEVERITY

### CRITICAL
None. (No look-ahead; cash+MTM reconcile over a lot's life — see reconciliation note below.)

### HIGH

**H1 — `record_every_n>1` desynchronizes per-step columns from `nav`.**
File: backtest.cpp:1289-1346 (fixed), 1682-1819 (strategy); tearsheet.cpp:63-163; run_report.cpp:190-235.
Problem: `nav += step_total` runs on EVERY step (1325,1792) but rows are pushed only when
`(i%stride)==0 || is_last` (1332,1796), and each pushed row carries only the CURRENT step's
`pnl_total`/`pnl_delta`/…/`pnl_settlement`/`pnl_shares`/`financing`/`cost`. Intervening steps'
P&L is dropped from the columns but kept in `nav`. Consequences: `Σ pnl_total(rows) ≠ nav.back()`;
`tearsheet` ann_return/ann_vol/sharpe/hit_rate (mean/std of the sampled `pnl_total`), the
attribution `col_sum`s (attr_delta…attr_cost), and `result_summary_metrics::avg_daily_pnl` are all
biased. `nav.back()`/`total_return` and `max_drawdown` (which read `nav`) remain correct because the
last step is force-recorded.
Impact: silently wrong headline risk/return stats under a supported config; the columns look
authoritative. Anyone downsampling a long run gets a corrupted tearsheet.
Fix: on a non-recorded step, accumulate the per-axis P&L into pending accumulators and flush the
SUM into the next recorded row (so each recorded row's `pnl_*` covers its whole block), OR document
loudly + have `tearsheet`/`run_report` refuse/scale when `record_every_n≠1`. At minimum assert
`Σ pnl_total == nav.back()` in a debug gate.

### MEDIUM

**M1 — American early exercise / assignment not modeled.**
File: backtest.cpp:686-768 (compute_step settlement), 1750-1763 (cash settle).
Problem: the book is only ever (a) MTM'd at American marks, (b) roll-closed at marks, or (c)
settled at expiry intrinsic. No early exercise is ever taken (long deep-ITM around dividends) and a
short ITM American is never early-assigned. Continuous American MTM captures most of the
early-exercise premium via the mark, so P&L impact is bounded, but assignment risk on short legs and
discrete exercise cashflows are absent.
Impact: understates short-option tail/assignment risk; small P&L bias for dividend-sensitive names.
Fix: add an optional early-exercise/assignment model (exercise-boundary crossing → intrinsic
settlement + share delivery), at least for short legs.

**M2 — Synthetic model-tenor lots cannot settle (exact-timestamp gate).**
File: backtest.cpp:706-714; strategy.cpp `canonicalize_tenor` 50-71, `prepare_cohort` 834-840.
Problem: `compute_step` fails closed (`NotFound`) unless `lot.expiry_ts_ns == shifted.ts_ns()`
exactly. `DeclarativeStrategy` builds `expiry_ts_ns = base_ts + round(target_T·kNsPerYear)`, an
arbitrary ns instant that will not equal a later snapshot's real market-close `now_ts_ns`.
`snap_to_listed=true` is rejected (`NotImplemented`, strategy.cpp:352-360), so a listed expiry can't
be pinned in this path. Result: a `HoldToExpiry` declarative run aborts the whole backtest at the
first crossed expiry. Only RollAtHorizon / CloseAtHorizon (which close at marks before expiry) work.
Impact: fail-closed (loud), but HoldToExpiry is effectively unusable with the declarative
interpreter; a real usability trap. Fix: snap synthetic expiries onto clock timestamps, or reject
HoldToExpiry+synthetic-tenor at config time with a clear message.

**M3 — Free/mid fills by default; synthetic spread even when frictions are on.**
File: backtest.hpp:257-264 (`FrictionModel` default `SpreadKind::None`, all costs 0),
backtest.cpp:1532-1596.
Problem: default backtests fill entries and closes at the fitted MID mark with zero slippage/cost.
Opt-in frictions add a half-spread as a bps-of-mark or vega·vol_tick MODEL over a fitted MID surface
— there is no real bid/ask in the declarative engine (real quotes are the separate
`listed_opra.hpp` workflow). Impact: default P&L is optimistic; even the friction model is
synthetic. Fix: make a nonzero default spread or require an explicit acknowledgment; expose a route
to consume real listed NBBO in the strategy path.

**M4 — Account financing uses order-/tenor-arbitrary inputs.**
File: backtest.cpp:1723 (`base->surfaces().front().pricing().r`), 1744 (`bs->q_eff_at(0.25)`).
Problem: cash-balance financing rate is whichever surface is first in archive order; shares
dividend-carry uses a fixed 0.25y `q_eff` for all shares regardless of tenor. Impact: minor P&L
skew, non-obvious and archive-order dependent for `finance_premium`. Fix: finance cash at an
explicit configured risk-free rate; use each name's own carry.

### LOW

**L1 — `RunConfig::retain_position_frames` is dead.** Declared (backtest.hpp:317) but read nowhere
in `src/` (grep: only the header + the design doc). Remove or implement.

**L2 — Fixed-book overload ignores `financing.initial_cash`/ledger.** `cash` column hardcoded `0.0`
(backtest.cpp:1224); `initial_cash`/financing config silently no-ops here. Documented, but a config
field accepted and ignored. (Also `entry_price` in the fixed book is validated but unused in P&L.)

**L3 — Entry-fill vs P&L-base tier mismatch can create spurious day-1 unexplained P&L.**
`entry_price` = sizing-Greeks model mark at `sizing_execution` (ColdReference under
`fast_screen_cold_confirm`, strategy.cpp:447-452,471), while the step's `price_base` uses
`cfg.price.query_execution`. If the two routes differ, `price_base@t_i ≠ entry_price@t_i` shows up as
first-step `unexplained` (no look-ahead — same date). Fix: price the entry base on the same route as
the P&L base.

**L4 — Residual hedge shares linger under `Cadence::AtEntry`.** When all options on a uid close, the
hedge shares are only flattened on the next hedge pass that FIRES; with `AtEntry` cadence and no
later entry, they persist and keep accruing `shares_pnl`/borrow. (backtest.cpp `hedge_daily`
447-491; `hedge_fires` gate 1492-1495.) Daily cadence is unaffected.

### INFORMATIONAL / POSITIVE

- **Determinism looks solid.** Reductions are fixed-input-order serial scatters
  (`reduce_pnl_totals` portfolio_pricer.cpp:1452; scenario_grid.cpp:263-278; pnl_attribution.cpp:180),
  ledgers iterate in insertion order (HedgeLedger `entries()`/`sum()` 419-431), and
  `unordered_map/set` are used only for membership/slot lookup, never iteration. Signal series
  NaN-fill absent names deterministically. Claimed bit-identical-across-threads is credible.
- **Reconciliation holds.** Over a lot's life: open `cash −= entry_premium` (no P&L); MTM accrues
  `Σ(mark_target − mark_base)` into `nav`; expiry `nav += (intrinsic − last_mark)` and
  `cash += intrinsic`. Net `nav` and net `cash` each `= intrinsic − entry_premium`. Frictions hit
  cash exactly once (`cash −= ex.cost`, 1613) and `nav` once (`step_total −= ex.cost`, 1791). No
  double counting between `settlement`(PnL) and the cash-settle loop, nor between `shares_pnl`(MTM)
  and hedge cash flows.
- **Incremental pricing is reasonable.** `RetainedBookPricer::retime` avoids a Portfolio rebuild
  when the book is unchanged (73-131); settlement marks are batched in one Marks pass (738-769);
  one full-book risk frame feeds entry vega + hedge delta + row greeks (1499-1510); roll-close marks
  reuse the P&L target solve. A daily-restrike still pays a Portfolio rebuild each entry step (book
  identity changes) — inherent, not a defect.

---

## FEATURE GAPS vs SOTA options backtester

- Real bid/ask / listed-quote fills in the declarative engine (only a synthetic spread model over a
  fitted mid surface; real quotes live in the separate `listed_opra` path).
- American early-exercise & assignment modeling; pin risk / settlement-price uncertainty at expiry;
  physical (share-delivery) settlement.
- Margin / capital / buying-power model and loop-enforced risk limits (no capital field exists;
  no position/greek limit is checked in the loop).
- Per-name hard-to-borrow / locate cost (only a flat `borrow_rate` proxy).
- Corporate actions: splits / special dividends / symbol changes (uid+strike continuity would break;
  only continuous `q_eff` carry + fit-time discrete cash divs are handled).
- Walk-forward / rolling out-of-sample harness and Monte-Carlo path overlay integrated into the time
  loop (`scenario_grid` exists but is a standalone single-date risk grid, not wired into the driver).
- Benchmark-relative statistics (beta/alpha/information ratio/tracking error) — tearsheet is
  absolute-only.
- Multi-leg ATOMIC fills / all-or-none (legs fill independently at mid; no cross-leg fill risk).
- Correct per-step P&L retention under downsampling (see H1) — a SOTA engine keeps block-summed
  attribution.
