# SPY Surface Backtest Hot-Path Profile

**Date:** 2026-07-10  
**Commit profiled:** `e53e026` plus compile-time phase instrumentation  
**Run:** `surface-smoke-v6`, 3 dates, 11 underliers, 22 held option legs

## Measurement contract

- Release `clang-cl` build on the pinned i7-1260P host.
- Ten steady repetitions after one discarded warm-up.
- `ATX_VOL_PROFILE=ON`, `ATX_VOL_COUNTERS=OFF` for phase timing.
- A separate `ATX_VOL_PROFILE=ON`, `ATX_VOL_COUNTERS=ON` run for algorithm counts.
- Phase timers are inclusive, so nested percentages do not sum to 100%.
- Process CPU time comes from `Process.TotalProcessorTime`; Windows reports it at
  coarse scheduler-tick granularity for this short process.

The production build keeps both options OFF. `ATX_VOL_PROFILE_SCOPE(...)` expands
to `((void)0)` and the clock, scope object, atomics, and timer storage are excluded.

## CPU and wall time

| Measure | Steady median |
|---|---:|
| Process wall time | 212.35 ms |
| Process CPU time | 328.13 ms |
| Instrumented engine wall time | 172.67 ms |
| Effective average CPU parallelism | 1.55 cores |

Process startup, run-spec/manifest parsing, and TSV output account for roughly 40 ms
outside `run_backtest` on this very short run. They amortize over a 60-date corpus.

## Phase profile

| Inclusive region | Calls | Median ms | Median % of engine |
|---|---:|---:|---:|
| Execution | 3 | 56.31 | 32.33% |
| Signals | 3 | 49.45 | 30.58% |
| Hedge risk, inside execution | 33 | 44.32 | 24.75% |
| Strategy step | 3 | 25.09 | 15.21% |
| Strategy book build, inside strategy | 1 | 23.53 | 14.32% |
| Snapshot load | 3 | 14.21 | 9.02% |
| Entry risk, inside execution | 22 | 11.93 | 7.73% |
| Book Greeks | 3 | 11.63 | 7.14% |
| Step P&L explain | 2 | 8.41 | 5.29% |
| Archive open | 3 | 8.22 | 5.26% |
| Archive map/reconstruct | 3 | 5.60 | 3.28% |
| Strategy entry marks | 1 | 1.37 | 0.86% |

## Algorithm counters

| Counter | Value | Interpretation |
|---|---:|---|
| Boundary solves | 1,825 | All-American cold pricing; no correction-cache hits |
| Premium quadrature evaluations | 50,672 | Dominant numerical kernel work |
| Normal CDF calls | 1,590,544 | Boundary/premium kernel cost |
| Log / exp calls | 751,472 / 1,502,944 | Boundary/premium kernel cost |
| Prepared portfolio builds | 38 | Rebuilt for every hedge UID, row risk, and P&L step |
| Frame column allocations | 542 | 33x14 hedge + 3x14 row Greeks + 2x19 P&L |
| Frame bytes | 19,536 | Materialized even though the driver mostly needs totals |
| Worker launches | 15 | Persistent pool initialization |
| Pool dispatches | 10 | Whole-book price/P&L fan-outs |
| Cache hits | 0 | Every American solve follows the cold route |

The 1,825 boundary solves reconcile to the current call graph. In particular,
`dispersion_signal` computes call and put American Greeks for every member even
though implied correlation consumes only ATM IV. Entry construction then computes
projected Greeks, marks, execution risk, hedge risk, and row risk again.

## Worker-count matrix

Profile-only medians, five steady runs per setting:

| Workers | Process wall ms | Process CPU ms | Engine ms | CPU / wall |
|---:|---:|---:|---:|---:|
| 1 | 230.97 | 234.38 | 190.63 | 1.01 |
| 2 | 231.61 | 265.63 | 194.73 | 1.15 |
| 4 | **221.65** | 265.63 | **179.61** | 1.20 |
| 8 | 241.14 | 328.13 | 201.59 | 1.36 |
| 16 | 257.20 | 343.75 | 207.18 | 1.34 |

Automatic 16-worker fan-out is too wide for 22 legs. Four workers are the current
latency optimum. Re-evaluate this policy on the 102-leg core book after eliminating
duplicate pricing; do not hard-code four globally from this development slice.

## Ranked next work

### P0: Make `dispersion_signal` IV-only

Split signal resolution from sizing resolution. Implied correlation needs forward
and ATM IV, not call/put American Greeks or straddle vega. `build_dispersion_book`
will still validate projected American vegas before sizing. This removes the daily
signal's 462 boundary solves and another 154 legacy signal solves at entry, targeting
the current 49.45 ms signal region and part of the 23.53 ms book build.

### P0: Fuse hedge risk and row Greeks

Price the full option book once on each date, aggregate delta by UID for the hedge,
and reuse the same frame totals for the recorded row. Today the engine creates and
prices 11 two-leg portfolios per date and then prices the 22-leg portfolio again.
This should remove 33 prepared builds, 462 frame-column allocations, and one of the
two daily full-Greek passes, targeting roughly 45-55 ms per three-date run.

### P1: Reuse entry projection results

`build_dispersion_book` already computes the projected call/put Greeks used for vega
sizing. Carry the marks and vegas into lot materialization/turnover accounting, or
materialize one batched entry frame. Avoid 22 scalar entry marks and 22 scalar entry
Greek calls. Target the 11.93 ms entry-risk region plus 1.37 ms entry marks.

### P1: Retain portfolio/pricer/workspace between rolls

The held book is immutable between rolls, but `compute_step` and `book_greeks`
recreate `Portfolio`, `PortfolioPricer`, `PreparedPortfolio`, workspace, and frames.
Retain them by a deterministic book fingerprint, use `pnl_totals`/`price_totals`, and
fall back to a diagnostic frame only when `n_ok` indicates an error. Rebuild only at
entry/roll. This removes the remaining five prepared builds and 80 frame-column
allocations on this run and becomes more important over 60 dates.

### P2: Size-aware worker policy

Cap worker fan-out by unique-contract/group count and measured grain size. The
22-leg book prefers four workers; 16 workers add about 15% engine latency and 29%
process CPU versus four. Rebenchmark after fusion on both 22- and 102-leg books.

### P3: Archive loading

Archive open/map is only about 9% now. Defer archive caching, async prefetch, or a
zero-copy mapped surface view until duplicate American pricing is removed. It may
become visible at 51 names, but it is not the present first-order bottleneck.

## Expected combined effect

The first four items remove redundant work rather than weakening the American model.
They should eliminate well over 40% of current boundary solves and most transient
portfolio/frame construction. The acceptance requirement is byte-identical P&L,
Greeks, hedge shares, NAV, and non-timing artifacts across the old and fused paths.
