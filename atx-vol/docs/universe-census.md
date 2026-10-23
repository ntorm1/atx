# Universe census (lane U0, vol-sota r6, 2026-10-23)

What the label-free universe is, how it is run under declared bounds, what this binary supports and refuses on it,
what it costs, and what a whole-universe latency claim would have to declare. Master plan Sprint 6 package U0,
gate G7. Lane report: `.superpowers/sdd/2026-10-18-vol-sota/lane-U0-universe-r6-report.md`; evidence:
`C:\atx-cache\quote-benchmark\U0-20261023-universe\` (`sha256.txt`).

**Scope of every number here.** One binary (`atx-vol-quote-bench` at `989e7e96` = root `30a0b661` + lane P0; no C3,
K5, K4b or H1), one shared development host (i7-1260P, 16 logical CPUs, up to five lanes running), one snapshot.
These are development rows, never bar results. Nothing is scored: the dataset has no labels, and no SpiderRock mark,
vendor score or holdout membership is read.

## 1. The universe

`C:\atx-cache\quote-benchmark\universe-20261014\` (lane L5u): the SpiderRock 2026-10-14 10:30 ET bucket, cut by the
unchanged quote-bench adapter with its 20 input columns only, `split = universe` on every row.

| | |
|---|---|
| Rows | 2,082,476 |
| Underliers | 6,067 |
| Chains (underlier, root, snapshot, expiry) | 37,086 |
| Surfaces (underlier, root) | 6,337 |
| Two-sided contracts | 1,580,163 (American 1,480,862) |
| `quotes.parquet` / `quotes.csv` / `manifest.json` sha256 | `09d0e800…` / `2c499e73…` / `bc673829…` |

`universe_census.py verify` recomputes all of these from the files (`E\manifest\`). Older roadmap counts refer to
different inventories (the 2026-08-21 OPRA board had 6,189 underliers).

## 2. Strata

Chain-level strata (`tools/universe_census.py`):

| Axis | Classes |
|---|---|
| U, underlier class (two-sided contracts of the underlier) | U1 ≥ 500, U2 100-499, U3 25-99, U4 < 25 |
| C, chain size (two-sided contracts of the chain) | C0 0, C1 1-10, C2 11-40, C3 41-100, C4 > 100 |
| E, expiry count of the underlier | E1 ≤ 3, E2 4-5, E3 6-11, E4 ≥ 12 |
| D, cash dividend | D1 when any row has `ddiv_pv > 0` |
| T, days to expiry (`expiry_days` is an epoch-day date) | T0 0DTE, T1 1-3, T2 4-30, T3 31-365, T4 > 365 |

Events cannot be stratified: the label-free dataset carries no event calendar. 230 of the 800 composite strata are
non-empty.

## 3. How it runs

- **Unit.** A job is a set of whole underliers (at most 24 chains, or one larger underlier alone), because
  `fit_quote_surface` fits each (underlier, root, snapshot) surface jointly. Jobs are cut from `quotes.csv` byte for
  byte (`write_job_csvs`) and run by `atx-vol-quote-bench --timing-detail`, one fresh process per job.
- **Order.** Underliers are packed in `sha256(seed | underlier)` order, so a run stopped at its cap leaves a uniform
  random subset of the board, not a biased one.
- **Checkpoint.** `jobs.jsonl` gets one line per finished attempt. A job with a final line is never re-run. A job
  killed at a chunk's end gets `chunk_stop` (not final) and re-runs in the next chunk in a new attempt directory
  (`out/<job>/a<N>`); nothing is overwritten.
- **Bounds** (declared before the first run; see the lane report):

| Bound | Value | Enforcement |
|---|---|---|
| Per-chain timeout | 20 s | Job budget of 20 s x chains, killed at the budget. A killed multi-underlier job is retried as single-underlier jobs; a single underlier killed is `timeout`. A completed chain whose own wall is over 20 s counts as `over_bound` |
| Memory | 768 MiB working set per process; host floor 1.5 GiB available | Sampled every 100 ms, killed above the cap (`memory_cap`); the OS peak is checked after exit; no launch below the floor |
| Concurrency | K = 4 processes x `ATX_VOL_FIT_WORKERS=2` (8 fit contexts) | Runner |
| Run length | ≤ 10 min per invocation; the full pass ≤ 60 min in total | No launch after 9 min, kill at 9.75 min; the cumulative chunk wall is refused past the cap |

- **Commands.**
  ```
  python -E -P -B universe_census.py verify --dataset <universe> --out <dir>
  python -E -P -B universe_census.py plan --dataset <universe> --out <dir> --mode sample|full --seed <seed>
  python -E -P -B universe_census.py run --plan <plan.json> --state <dir> --binary <quote-bench> \
      --policy <policy.json> --workers 2 --k 4 -- --dividends funding-pv --timing-detail
  python -E -P -B universe_census.py summarize --plan <plan.json> --state <dir> --out <summary.json>
  python -E -P -B universe_census.py compare --plan <plan.json> --a <state> --b <state> --out <compare.json>
  ```
  `-E -P`, not `-I`: polars and psutil live in the user site on this host.

## 4. The cost model per chain

From `--timing-detail` (`graded_stats.slices[].timing`, exclusive buckets of the chain's own wall):

| Component | Buckets |
|---|---|
| family / setup | `deam_family_build_us` (inside the Deam bucket: `ensure_families` runs inside `DeAmIvView::invert`) |
| root (inverses, endpoints, reference route) | `deam_us - deam_family_build_us` |
| fit | `solve_us + project_us` |
| calendar | `calendar_us` |
| final grid | `price_us + certify_us` |
| record | not in this binary's fit path: H0-A serve records are not built by quote-bench (see the query load) |
| fallback | counted, not timed per chain: reference-route requests / evaluations, Premium fallbacks |

## 5. Results on `989e7e96` (2026-10-23, development host)

Policy: the K4 speed arm `graded-v1-famq-grid-native-route.json` (sha256 `1a488f48…`), `--dividends funding-pv`. On the
sample the promoted default `graded-v1.json` serves exactly the same rows (0 lost, 0 gained, 0 status changes).

**Runs.**

| Run | Chains | Config | Wall | Chains/s | Outcome |
|---|---:|---|---:|---:|---|
| Stratified sample (231 underliers, 230 / 230 strata, 4.04 %) | 1,499 | 4 processes x width 2 | 12.2 s | 123.0 | 72 / 72 jobs ok |
| Same sample, all cores in one process | 1,499 | 1 x width 16 | 33.5-34.2 s | 44 | identical rows |
| Full board, one bounded pass (cap 60 min) | 37,086 | 4 x width 2 | 322.2 s | 115.1 (134.1 without the query load) | 1,808 / 1,808 jobs ok, coverage 100 % |

No exception, crash, timeout, `over_bound` chain or memory-cap kill occurred anywhere. Peak working set per
process: p50 9.2 MiB, max 33.9 MiB (cap 768 MiB).

**Support.** 1,590,554 rows served (76.4 % of rows, 82.5 % of eligible), each with an identified IV. Chains: graded
A 7,198 · B 20,792 · C 2,348. Refused: calendar_arbitrage 2,946 · arbitrage_failure 2,019 · insufficient_quotes 278.
Not attempted: 1,505 (European 491, 0DTE 36, adjusted deliverables 695, other input rejections). Row refusals:
calendar_arbitrage 167,615 · input_rejected 155,506 · arbitrage_failure 124,900 · unsupported_deliverable 25,202 ·
invalid_volatility 14,391 · insufficient_quotes 2,774 · invalid_market 1,534.

The largest refusal classes are the American calendar price crossing (2,946 chains) and the low-price nondividend
"scaled European upper bound" check (1,147 chains). Served row share by stratum: U1 70.4 %, U2 85.2 %, U3 80.6 %,
U4 54.4 %; E1 12.1 %; C4 58.7 %; T1 66.4 %; D1 84.5 %.

**Cost per chain** (35,581 fitted chains, ms):

| | p50 | p90 | p99 | max | share |
|---|---:|---:|---:|---:|---:|
| root | 12.7 | 48.0 | 140.5 | 1,008.6 | 45.6 % |
| final grid | 8.0 | 51.2 | 171.7 | 1,650.5 | 43.7 % |
| calendar | 0.01 | 11.8 | 71.6 | 855.9 | 9.2 % |
| family / setup | 0.0 | 0.0 | 14.4 | 34.5 | 0.8 % |
| fit | 0.12 | 0.35 | 3.06 | 41.5 | 0.5 % |
| chain wall | 24.5 | 109.7 | 315.3 | 2,670.2 | Σ 1,674 s |

Whole board ≈ 28 CPU-minutes of chain work, plus about 94 ms of process start, read and write per job.

**Determinism.** The sample's re-run (under the query load), the all-cores runs and the first run share the
combined served-row hash `0286ffe5…` (72 / 72 jobs). Every sample row is also byte-identical in the full pass, where
it sits in different jobs. Full board: `3dc91f72…` over 1,808 jobs.

## 6. Simultaneous quote-query load

`atx-vol-quote-query-load` runs 20,000 queries/s for 300 s on H0-A records of 4 SPY expiries. A publisher re-fits and
republishes back to back (19 versions in the window). The universe sample re-run and the full pass ran beside it.

| | |
|---|---|
| Queries / torn / stale | 6,000,000 / **0** / **0** |
| Service p50 / p99 / p99.9 | 363 ns / 14.4 µs / 26.8 µs (Ladder 331 ns / 1.61 µs / 4.95 µs; Family 10.7 µs / 22.0 µs / 74.9 µs) |
| Response p50 / p99 / p99.9 / max | 451 ns / 625 µs / 17.3 ms / 34.0 ms (deadline > 1 ms: 0.88 %) |
| Miss rate (status not Ok) | 8.42 %, all `Exercised` (intrinsic with exercise Greeks); unanswered 0 |
| Rebuild completion | p50 17.4 s (fit 12.6 s at width 2, records 4.25 s) |
| P0 pool queue age (pass A, width 2, 8 expiries) | p50 541 ms, p99 1.42 s, max 1.59 s |
| Fit throughput loss | sample −11.7 %; full pass −21.8 % while queried |

The record path stays sub-microsecond on ladders under load. The response tails come from OS preemption of an
unpinned thread on an oversubscribed shared host: a query-only window has p99 ≤ 98 µs. Pinning the query thread onto a
busy CPU made it far worse. A production quoting thread needs a core reserved away from the fit pool.

## 7. Whole-universe latency: the contract a claim must declare

Nothing here supports a whole-universe subsecond claim, and none may be inferred from the single-core SPY milestone
(F1) or from P0's F3. A claim must declare:

1. **Machine.** Core count and type, SMT, memory and the process topology. Here, 4 processes x 2 fit workers on a
   shared i7-1260P gave 322 s for the board. At 28 CPU-minutes of chain work, 1 s needs about 1,700 fully busy
   contexts at today's per-chain cost, or a matching per-chain speedup.
2. **Output.** Which rows count: served (76.4 % here), refused with a typed status, or unsupported (European,
   0DTE, adjusted deliverables). The policy (K4 arm vs the default) and the dividend convention (funding-PV
   approximates dated cash).
3. **Clock.** Cold calibration from a resident snapshot (as B0) or process wall, and whether serve records count.
   Records are not built in this census; H0-A's cost is about 1 s per expiry for ladders.
4. **Parallel shape.** Universe surfaces are small (modal 4-5 expiries). In-process width barely helps (one
   process at width 16 = 44 chains/s vs four processes at width 2 = 123 chains/s), so the claim must say how
   surfaces are spread over cores.
5. **Tails.** The per-chain p99 is 315 ms and the max 2.67 s (OEX). A deadline per surface needs its own bound.
