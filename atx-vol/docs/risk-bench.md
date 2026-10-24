# atx-vol-risk-bench

Intraday benchmark of the atx-vol risk engine (design
`docs/superpowers/specs/2026-10-24-atx-vol-risk-engine-design.md` §5.7, §5.8): a fixed
book of listed options and share legs over 100 underliers, explained step by step
across SpiderRock 30-minute snapshots (10:00 → 15:00 ET = 11 states, 10 steps). It
reports per-stage clocks, throughput, explain quality, vendor parity, thread
determinism and, on request, the cold-kernel comparison.

The licensed SpiderRock store and the synthetic store go through the **identical**
loader and engine path; only the directory differs (design D7).

Source: `tools/risk_bench_main.cpp` (argv → run → write), `tools/risk_bench.cpp`
(`run_risk_bench`, drives `RiskStream`), `tools/risk_bench_core.cpp` (engine-free core:
CLI, book selection, statistics, JSON / markdown; also compiled into `atx-vol-tests`),
`tools/risk_bench.hpp`. Loader / synthetic store:
`include/atx/vol/api/marketdata/spiderrock_store.hpp`,
`src/marketdata/{spiderrock_store,synthetic_spiderrock_store}.cpp`.

## Running it

### Licensed store (trading date 2026-10-14)

The 30-minute drop `OptionIntradayHist_30min_<date>.parquet` is first partitioned into
the pinned store layout (`<root>/date=<d>/bucket_et=<HHMM>/data_0.parquet` + manifest):

```powershell
python -I atx-vol\scripts\spiderrock_intraday_to_store.py --parquet <OptionIntradayHist_30min_<date>.parquet> --out C:\atx-data\spiderrock-30min
atx-vol-risk-bench --store C:\atx-data\spiderrock-30min --date 2026-10-14 --from 1000 --to 1500 `
                   --underliers 100 --options 10000 --shares 100 --kernel both --threads 8 `
                   --determinism --out risk-bench-2026-10-14.json --md risk-bench-2026-10-14.md
```

The tool never reads the single-file drop directly. On this host the partitioned store
holds 20 slices of 2026-10-14 (0930 … 1630; 2,082,476 rows per regular slice, 6,067
underliers).

### Synthetic store (no licensed data)

```powershell
atx-vol-risk-bench --synthetic C:\tmp\sr-synthetic --from 1000 --to 1500 --underliers 100 `
                   --options 10000 --shares 100 --kernel family --threads 8 --determinism `
                   --out synthetic.json --md synthetic.md
```

`--synthetic <dir>` writes the store there first (date = `--date`, `--underliers`
names, the default 11 buckets 1000 … 1500, seed = `--seed`) unless the directory already
holds a bucket of that date, then reads it like any store. Full size (100 × 8 expiries ×
25 strikes × 2 sides × 11 buckets = 440,000 rows) takes **14.6 s** on the i7-1260P with
two other build lanes running (pricing 12.5 s, fanned over the pricing executor).

### CLI

| flag | default | meaning |
|---|---|---|
| `--store <root>` | — | store root (holds `date=<d>/bucket_et=<HHMM>/`) |
| `--synthetic <dir>` | — | write the synthetic store there (unless present) and read it; exclusive with a different `--store` |
| `--date YYYY-MM-DD` | `2026-10-14` | trading date |
| `--from HHMM` / `--to HHMM` | `1000` / `1500` | bucket window, inclusive; needs ≥ 2 buckets |
| `--underliers N` | 100 | top-N underliers by (filtered) row count at the first bucket |
| `--options N` | 10000 | option legs, split evenly over the underliers |
| `--shares N` | 100 | share legs, one per underlier (first N underliers) |
| `--kernel family\|cold\|both` | family | the stream's kernel; `both` adds a cold stream for comparison |
| `--threads N` | 8 | kernel and explain fan-out degree (0 = executor auto) |
| `--needs rho\|none` | none | evaluate the rho axis (carry-bump families) |
| `--time voltime\|calendar365` | calendar365 | the clock every state (and so the kernel) measures T on (see "Clock") |
| `--trapezoid` | off | `ExplainOptions::trapezoid_second_order` |
| `--determinism` | off | a threads = 1 shadow stream; totals must be bit-equal |
| `--out <file>` | stdout | JSON report |
| `--md <file>` | — | markdown summary |
| `--dump-legs <file.csv>` | — | per-option-leg CSV (see "Per-leg dump" below) |
| `--dump-steps N` | 1 | steps written to `--dump-legs`: the first N, 0 = every step |
| `--seed N` | 1 | book selection seed |
| `--n-sigma N` | policy (12) | family kernel: sigma-node ceiling `FamilyKernelPolicy::n_sigma` |
| `--n-sigma-min N` | policy (5) | adaptive node floor `n_sigma_min`, capped at `n_sigma` (both floors ≥ `n_sigma`: the fixed count) |
| `--sigma-box-pad X` | policy (0.05) | sigma box pad |
| `--sigma-node-spacing X` | policy (0.06) | adaptive log-sigma node spacing |
| `--family-economics on\|off` | policy (on) | route groups whose family cannot amortize to cold-analytic (`FamilyUneconomic`) |
| `--family-node-cost-us X` | policy (73) | economics: cost of one sigma node |
| `--cold-lane-cost-us X` | policy (270) | economics: cost of one cold-analytic lane |
| `--family-schedule static\|dynamic` | policy (dynamic) | block fan-out: `run_blocks` / `run_dynamic` |
| `--short-box-tau X` | policy (0.3) | blocks with T < X years take the short-tenor floor; 0 = off |
| `--n-sigma-min-short N` | policy (4) | the short-tenor node floor `n_sigma_min_short`, capped at `n_sigma` |
| `--help` | | usage |

Exit codes: **0** success, **1** runtime error (missing store, engine error, write
failure), **2** usage error. Progress goes to stderr.

The family-kernel flags (lane A2) override single `FamilyKernelPolicy` fields; an absent
flag keeps the policy default shown. They are parsed for syntax only (an unsigned or a
finite number, `on|off`, `static|dynamic`): the kernel validates the ranges, so an
out-of-range value fails the run (exit 1), not the parse. The effective policy is echoed
in `inputs.family_policy` and as the markdown line `- family policy: …`.

The carry mode has no flag: `--store` builds states with `ImpliedFromCalls`, `--synthetic`
with `Published` (its vendor columns are priced on exactly the D4 carry, design D20); see
"Carry modes". Both are echoed in the report (`inputs.time`, `inputs.carry_mode`).

## Loader conventions (`load_spiderrock_bucket`, `market_state_from_bucket`)

Columns read: `undSecKey_tk, okey_cp, okey_yr, okey_mn, okey_dy, okey_xx, uPrc, rate,
sdiv, ddiv, years, srVol, srPrc, de, ga, th, ve, bidPrc, askPrc`, plus the optional
filter columns `exType`, `prcFramework` and `okey_tk` and the optional numerics `ddivPv`,
`synSpot` and `yearsC` when present; every other column is tolerated and ignored. An
optional numeric becomes `SpiderRockBucket::ddiv_pv` / `syn_spot` / `vendor_years_c`: one
value per row (NaN for a NULL / -99 cell and for the rows of a file without the column),
EMPTY when no file of the partition has it. Strings may be `utf8` (the partitioned
2026-10-14 drop) or `large_utf8` (the polars-written oracle store); numerics must be
Float64, okey_yr/mn/dy Int64 (Int32
accepted). Only `<root>/date=<d>/bucket_et=<HHMM>` is opened; row groups whose ticker
statistics exclude every admitted ticker are skipped. The schema must be flat (every
field one parquet leaf column): the projection and the statistics address leaves by
arrow field index, so a nested column is refused (`InvalidArgument` naming the file)
rather than misread or pruned silently.

- **expiry** = okey_yr/mn/dy at 16:00 ET (`expiry_instant_ns(…, Pm)`); **bucket instant**
  = 16:00 ET of the date minus (16:00 − HH:MM) (`spiderrock_bucket_instant_ns`; exact
  because DST resolves at day granularity). `years` is kept as `vendor_T` only; T is the
  state's `TimeSpec` clock (Calendar365 default).
- **Row accounting** (every row counted once):
  `rows_read == size + rows_dropped_null + rows_dropped_bad_input + rows_not_selected +
  dropped_non_american + dropped_nonstandard_root + dropped_forward_framework`.
  - `rows_dropped_null`: NULL ticker, okey_cp, okey_yr/mn/dy, okey_xx, uPrc, rate, years
    or srVol, or a vendor sentinel there (-99).
  - `rows_dropped_bad_input`: non-finite / non-positive strike, uPrc or srVol,
    non-finite rate / years, unparseable side or expiry.
  - `rows_not_selected`: underliers outside the admitted set (explicit list or top-N),
    including pruned row groups.
  - `dropped_non_american` (`SpiderRockStoreSpec::american_only`, default on): rows
    whose `exType` is not `American` (NULL included). **Applied before the top-N
    count**, so European / Forward-framework index products (SPX, NDX, XSP, RUT, …,
    114,149 rows of bucket 1000) never enter the bench universe: the risk kernel is
    American-on-spot and would fail vendor parity on them by construction. Explaining
    European index books is deferred (a Black-76 forward kernel is a separate
    decision). Absent column (synthetic / older oracle store) ⇒ every row is American.
  - `dropped_forward_framework` (`spot_framework_only`, default on): rows that passed
    the exType filter but whose `prcFramework` is not `Spot` (case-insensitive; NULL
    included). A Forward-framework row carries a per-EXPIRY forward in `uPrc` (OEX:
    3,878 / 3,929 / 3,960 at 2 d / 5 mo / 8 mo), so the per-uid median spot is
    meaningless to the American-on-spot kernel (the 12 largest |Δprice| of the first
    licensed parity run were all OEX). Applied after exType (European Forward-framework
    index rows stay in `dropped_non_american`) and before the root filter and the top-N
    count. Bucket 1000 of 2026-10-14: **4,736** rows (OEX 4,064, XAU 672; the brief
    expected OEX's 4,064 and did not count XAU, the other American Forward-framework
    underlier). Absent column ⇒ nothing dropped.
  - `dropped_nonstandard_root` (`standard_roots_only`, default on): rows whose option
    root `okey_tk` differs from the underlier ticker — adjusted / non-standard
    deliverables. Keyed by underlier their (expiry, side, strike) collides with the
    standard contract (bucket 1000: 16,355 colliding keys, every one a distinct root;
    31,329 rows on 291 underliers) and their price is not a function of the
    underlier's spot. Also applied before the top-N count; 61 underliers that list only
    non-standard roots disappear.
  - `rows_null_vendor` (kept rows, not part of the identity): an optional vendor /
    carry cell (srPrc, de, ga, th, ve, sdiv, ddiv, bidPrc, askPrc) is NULL or a sentinel
    (-99 anywhere; `sdiv <= -9.98`, the drop's -9.99 sentinel) and is carried as NaN.
- Rows are sorted by (uid, expiry_ns, side, strike); uids are interned in ascending
  ticker order in a run-wide `UnderlierTable`, so they are stable across buckets.
- **Top-N** (`max_underliers`): by row count over the filtered rows, ties by ticker.
  The bench fixes the universe at the first bucket and reads later buckets with that
  explicit ticker list.

### Clock (`--time`)

`T` is the state's `TimeSpec` clock; the vendor's `years` is kept only as `vendor_T`.
`SpiderRockStoreReal.LoadsBucket1000` measures |T − `years`| over the 365,600 top-100
marks of bucket 1000 (days, 1 d = 1/365.25 y; median / p99 by calendar tenor):

| clock vs | all | ≤ 1 d (3,672) | 1–7 d (30,318) | 7–30 d (65,058) | > 30 d (266,552) |
|---|---|---|---|---|---|
| VolTime − `years` | 0.919 / 2.368 | 0.008 / 0.008 | 0.004 / 0.017 | 0.011 / 0.014 | 0.933 / 2.368 |
| Calendar365 − `years` | 1.245 / 3.531 | 0.695 / 0.695 | 1.250 / 1.250 | 1.559 / 1.688 | 1.242 / 3.531 |
| VolTime − `yearsC` | 0.919 / 2.368 | 0.937 / 0.937 | 0.004 / 0.017 | 0.011 / 0.014 | 0.933 / 2.368 |
| Calendar365 − `yearsC` | 1.245 / 3.531 | 0.250 / 0.250 | 1.250 / 1.250 | 1.559 / 1.688 | 1.242 / 3.531 |

(`yearsC` equals `years` on this drop except on the same-day expiry, where it is 0.)
The per-expiry lines the test prints locate the VolTime gap exactly: ≤ 0.02 d on every
expiry up to 2026-11-20, then **+0.46–0.48 d at each NYSE early-close session** the
interval crosses (2026-11-27, 2026-12-24, 2027-11-26, 2028-07-03, 2028-11-24: +0.48 d at
2026-11-27, +0.95 d at 2026-12-31, +1.42 d at 2027-12-17, +1.87 d at 2028-09-15, +2.37 d
at 2028-12-15). A 13:00 close removes 3 trading hours (3 × 0.7 / 1638 y = 0.468 d);
`VolTimeCalendar` holds full closures only. The overall VolTime median (0.919 d) misses
the 0.5 d adoption bar, so **calendar365 is the default under both stores** and `--time
voltime` is opt-in. Reproducing `years` needs either an early-close table in
`VolTimeCalendar` (core clock change; the per-expiry lines suggest it would bring every
tenor to ≤ 0.02 d) or a per-expiry T override carried by the state (lane A header: e.g. a
vendor T per `ExpiryCarry` row that the kernel and the explain's dt use instead of
`time_to_expiry_years`) — an orchestrator decision. The synthetic store's `years` IS the
Calendar365 T, so calendar365 is also the only clock its vendor Greeks reproduce.

### State per bucket and the carry modes

Per uid: `spot` = median uPrc over its rows (rows are captured at slightly different
timestamps inside a slice), `flat_rate` / `flat_q` = medians of rate / sdiv. The state
carries one `ExpiryCarry{expiry_ns, pv_divs, rate, q}` per (uid, expiry); its basis is
`SpiderRockBucket::carry_mode` (copied from `SpiderRockStoreSpec::carry_mode`, default
`ImpliedFromCalls`; a hand-built bucket defaults to `Published`).

#### `Published` (design D4)

SpiderRock publishes one (rate, sdiv, ddiv) tuple per **expiry**: `ddiv` is the cash
dividend amount paid before that expiry and `ddivPv` its PRESENT value. The row is the
medians over that expiry's rows — `pv_divs` = median `ddivPv` when the column exists (an
expiry whose ddivPv cells are all NULL falls back to the ddiv median), else median `ddiv`
(`BucketStateReport::pv_divs_from_ddiv_pv` says which) — and the engine prices on

    F = (S − pv_divs) · e^{(r − q)·T},   q_eff = r − ln(F / S) / T

(`spiderrock_q_eff`). `cash_divs` stays empty. The median's loss is reported:
`BucketStateReport::max_ddiv_spread` (max over (uid, expiry) of max − min of the pv_divs
source column) — **0** on bucket 1000 of 2026-10-14 (constant within an expiry there).
Also reported: `n_uids_with_divs` (59 of the top-100 at bucket 1000 under Published, 26
under ImpliedFromCalls, whose implied rows carry pv_divs = 0), `n_expiry_rows`,
`n_duplicate_marks` (0 with standard roots only), `n_rows_bad_sigma`.

#### `ImpliedFromCalls` (the loader's default; the bench under `--store`)

Why: SpiderRock's `sdiv` is a signed RESIDUAL (DIA: −0.20 at 2 d → +0.011 at 2 y), its
discrete dividends are escrowed only up to some horizon with a continuous yield beyond
it, `ddiv` is published regardless, and which regime an expiry is in is not a column. On
DIA 2028-12-15 (T 2.17, ddiv 31.0, ddivPv 29.4, sdiv +1.07 %) the vendor's own calls
imply ONE forward, 562.2 flat across the 440 … 600 strikes (±0.01 %), = S·e^{(r−sdiv)T}
with NO escrow, while the published basis gives 530.4 (ddiv) / 532.1 (ddivPv) and
misprices the 480 call by 20 (ours 74.7, vendor 94.9, market 92–97).

Algorithm, per (uid, expiry):

1. Candidates: the expiry's CALLS with strike ≥ the uid spot, a two-sided quote
   (bid > 0, ask ≥ bid), a finite `srPrc` > 0 and `srVol` in (0.01, 3].
2. Each candidate is inverted for F in the European Black price
   `srPrc = e^{−rT}[F·N(d1) − K·N(d2)]` at σ = `srVol`, r = the expiry's median `rate`,
   T = the expiry's median vendor `years` (the clock `srPrc` was computed on). F is
   bracketed in [0.2, 5] × spot and solved to 1e-10 relative in ≤ 100 evaluations by a
   Newton iteration safeguarded by the bracket (a step leaving it bisects), warm-started
   at the Published forward and then at the previous call's F (`forward_from_call`).
   A target outside the bracket's price range never converges and is dropped.
3. F = the MEDIAN of the solved F; IQR = Q3 − Q1 with linear interpolation between
   order statistics at (n−1)/4 and 3(n−1)/4.
4. Accepted when n ≥ `kImpliedCarryMinCalls` (3) and IQR ≤ `kImpliedCarryMaxIqrRel`
   (1e-3) × F. The row is then `pv_divs = 0`, `rate` = median rate,
   `q = rate − ln(F / spot) / T_state` with T_state on the STATE's clock, so the engine's
   `spot·e^{(rate − q)·T_state}` = F exactly (whatever `--time` is).
5. Otherwise (too few calls, the IQR guard, T_state ≤ 0 or refused) the expiry keeps its
   Published row.

Reported in `BucketStateReport`: `n_expiries_implied`, `n_expiries_published` (their sum
is `n_expiry_rows`), `max_implied_vs_published_forward_rel` (max over implied rows of
|F_implied / F_published − 1|, F_published = the Published row's
(spot − pv_divs)·e^{(rate − q)·T_state}) and `implied_carry_seconds`. The
`market_state_from_bucket(bucket, time, report, forwards)` overload returns every row's
basis (`SpiderRockExpiryForward`: mode, implied F even when rejected, published F, n_calls,
IQR), which `--dump-legs` writes per leg.

Bucket 1000 of 2026-10-14, top-100: **1,758 expiries implied, 155 Published** (10 with
< 3 calls, 145 on the IQR guard — e.g. TLT / IWM / SPY long-dated, whose calls do not
imply one forward); DIA 2028-12-15 **F = 562.34** (IQR 3.9e-4, 55 calls; Published
532.14); largest gaps MULL 2028-01-21 +33.1 % (a 2× leveraged ETF; 14 calls, IQR 2.5e-4),
DIA 2029-01-19 +5.9 %, DIA 2028-12-15 +5.7 %, DIA 2028-06-16 +4.8 %; inversions
**12.5–16.8 ms** per bucket (~75k calls, host at ~95 % CPU from other lanes).

Marks: one `ContractMarks` block per uid, sorted by (expiry, side, strike), unique keys
(first row wins), sigma = srVol, vendor_price = srPrc, vendor_delta = de.

Measured on the licensed store, bucket 1000 (vol-rel, i7-1260P, shared with two build
lanes; `SpiderRockStoreReal.LoadsBucket1000`, env `ATX_VOL_SPIDERROCK_STORE`):

| load | wall | rows kept | dropped |
|---|---|---|---|
| uncapped | 1.9–2.9 s | 1,932,026 of 2,082,476 | null 236, bad 0, non-American 114,149, forward framework 4,736, non-standard root 31,329; 32,115 kept rows with a NULL / sentinel vendor cell; 5,954 underliers |
| top-100 | 1.9–3.3 s (+ state build 40–100 ms incl. the implied carry) | 365,600 (= marks) | non-selected 1,566,662; 1,913 ExpiryCarry rows (1,758 implied); 26 uids with pv_divs > 0 |

(Ranges over runs on the shared host.) The full benchmark book selects on this bucket
with no shortfall: 100 underliers, 10,000 option legs (202,936 eligible contracts),
100 share legs, ≥ 8 expiries and both sides per underlier
(`RiskBenchBookReal.SelectsFullBookOnBucket1000`).

## The synthetic store

`write_synthetic_spiderrock_store` (deterministic from `seed`; `std::mt19937_64` with
portable uniform / Box-Muller transforms, so bytes do not depend on the standard
library or the thread count). Per underlier `SYN0001…`: S0 ~ U[20, 400],
r ~ U[0.03, 0.05], q ~ U[0, 0.03]; an eSSVI smile per expiry with ATM vol in
[0.15, 0.6] (sqrt-T tilt), phi = eta / sqrt(theta), eta ~ U[0.4, 1.0], rho ~
U[−0.6, −0.1] plus a per-expiry jitter; 30% of underliers pay a quarterly cash dividend
whose PV to each expiry is `ddiv`. Expiries at 7, 14, 30, 60, 91, 182, 273, 365 days
(16:00 ET); strikes are listed contracts fixed at the first bucket (|z| ≤ 2.5 around F,
snapped to a decimal grid). Per bucket: GBM spot step at `spot_vol` over the bucket gap
on a trading clock (390 min × 252 d), additive ATM-level shock N(0, level_shock_vol) and
rho shock N(0, skew_shock_vol) as random walks, T decays with the clock (`years` =
Calendar365 T). Vendor columns are `american_greeks_al(S, K, T, srVol, r, q_eff, side,
al_fast_opts())` at the D4 carry — the preset the kernel resolves for a marks-only
state — written in SpiderRock units (`ve` per vol point = vega × 0.01, `th` per business
day = theta / 252). Bid / ask = price ∓ max(0.01, 2%) rounded to cents (bid 0 below one
cent: one-sided). Strings are large_utf8; the extra `atmVol` column is written.

## Book construction (deterministic from `--seed`)

On the first bucket: the top `--underliers` uids by row count (ties: lower uid);
`--options` split evenly (remainder to the first uids). Per uid, an eligible contract has
|vendor delta| in [0.05, 0.95], srVol in (0.01, 3.0] (the licensed drop carries
penny-name srVol ≥ 5 rows) and a two-sided quote (bid > 0, ask ≥ bid). Picks go
round-robin over the uid's expiries from an rng-chosen start; the side is chosen per
expiry, `(rank + picks) % 2`: inside an expiry the sides alternate, so each |delta|
target of the expiry's cycling grid (0.50, 0.25, 0.75, 0.10, 0.90, …) is taken as a call
AND a put, and the first pass over the expiries alternates from one expiry to the next,
so even a quota no larger than the expiry count holds both sides. Each pick is the
unused contract nearest the target; a quota of ≥ 4 spans ≥ 4 expiries (when the uid
lists them). `select_bench_book` refuses `--underliers 0` and a bucket whose uid /
expiry / side / strike / vendor delta / bid / ask column does not cover every row.
qty is uniform in
{−10 … −1, 1 … 10}, multiplier 100. Shares: one leg per uid for the first `--shares`
uids, qty = −round(Σ qty·100·vendor delta / 100)·100 (100 when that rounds to 0): a
delta-hedged book. With `american_only` the top-100 by row count are equity / ETF names
(SPY, QQQ, MU, SNDK, GLD, SMH, META, AMD, …).

## The run

`RiskStream` over buckets `--from … --to`: `start(state_0)` then `step(state_k)` per
bucket (one frame per state, one explain per step). Clocks per step: bucket load
(parquet + decode), state build (`market_state_from_bucket` + `make_market_state`),
frame evaluate and family build (`RiskFrame::counters`), explain
(`ExplainResult::explain_seconds`) and the whole `step` wall clock. `--determinism` runs a
second stream at threads = 1 on the same states and compares bit-for-bit the explain
totals, per-leg pnl_total / pnl_unexplained and the frame's price / delta / gamma /
vega / theta. `--kernel both` runs a cold stream alongside and reports the evaluate
speedup and the max |Δ| per column (`null` when no row is Ok in both frames: nothing
compared is not "no difference"). Only the main stream is timed in the step clocks.

Explain-quality statistics (diagnostic, **not** a regulatory number): per step, the
Spearman rank correlation and the two-sample KS statistic between per-leg explained
(pnl_total − pnl_unexplained) and actual pnl_total over Ok legs, graded with the FRTB PLA
cutoffs verbatim (design D9): green = Spearman > 0.80 and KS < 0.09; red = Spearman <
0.70 or KS > 0.12; amber otherwise. The run summary also grades the per-step TOTALS
series (needs ≥ 2 steps; with 10 steps it is a weak statistic).

Vendor parity, per step on the shifted frame's Ok rows flagged `VendorParityAvailable`:
our price vs srPrc, delta vs de, vega vs ve / 0.01; MAE, max |Δ| and the mean SYMMETRIC
relative error |m − o| / max(|m|, |o|, floor) with floors 0.01 (price), 1e-3 (delta,
vega). In synthetic mode this is an identity check up to the family kernel's accuracy.

## Report schema (JSON, `schema_version: 2`)

Non-finite numbers are written as `null` and read back as NaN; doubles use the shortest
round-trip decimal form, so `report_from_json(report_to_json(r))` re-serializes byte for
byte (`RiskBenchReport.JsonSchemaV2RoundTrip`). Times are seconds; P&L is
position-scaled currency (qty × multiplier).

**v2** (lane C follow-up 2) adds `inputs.time`, `inputs.carry_mode` and the
`bucket_loads[]` fields `dropped_forward_framework`, `pv_divs_from_ddiv_pv`,
`n_expiries_implied`, `n_expiries_published`, `max_forward_gap_rel`,
`implied_carry_seconds`; lane A2's family-kernel fields `inputs.family_policy`,
`steps[].frame.family_nodes` and `steps[].cold.max_rel_price_family` are v2 fields too.
The reader requires every v2 field and refuses any other `schema_version` (a v1 report is
a ParseError, never read with guessed defaults).

| path | type | meaning |
|---|---|---|
| `schema_version` | int | 2 |
| `tool` | string | `atx-vol-risk-bench` |
| `inputs.store` | string | store root read (the synthetic dir in synthetic mode) |
| `inputs.date`, `.from`, `.to` | string | trading date, bucket window |
| `inputs.synthetic` | bool | `--synthetic` given |
| `inputs.kernel` | string | `family` / `cold` / `both` |
| `inputs.threads` | uint | `--threads` |
| `inputs.needs_rho`, `.trapezoid`, `.determinism` | bool | flags |
| `inputs.seed` | uint64 | `--seed` |
| `inputs.requested_underliers`, `_options`, `_shares` | uint | requested book shape |
| `inputs.host_threads` | uint | `std::thread::hardware_concurrency()` |
| `inputs.time` | string | `calendar365` / `voltime` (`--time`) |
| `inputs.carry_mode` | string | `implied_from_calls` (`--store`) / `published` (`--synthetic`) |
| `inputs.family_policy` | string | the effective `FamilyKernelPolicy` of the main stream (`n_sigma`, `n_sigma_min`, pad, spacing, economics and its two constants, schedule, `short_box_tau` and `n_sigma_min_short`); empty under `--kernel cold` |
| `synthetic_store.generated` | bool | the store was written by this run |
| `synthetic_store.seconds`, `.rows` | number, uint | generation wall clock, rows written |
| `book.n_option_legs`, `.n_unique_options`, `.n_share_legs`, `.n_underliers` | uint | from `RiskBook` |
| `book.shortfall` | uint | requested option legs not found (thin underliers) |
| `buckets` | [string] | buckets in the window, ascending |
| `bucket_loads[]` | object | per bucket: `bucket`, `rows_read`, `rows_kept`, `rows_dropped_null`, `rows_dropped_bad_input`, `rows_not_selected`, `dropped_non_american`, `dropped_nonstandard_root`, `rows_null_vendor`, `n_uids`, `n_marks`, `n_uids_with_divs`, `max_ddiv_spread`, `dropped_forward_framework`, `pv_divs_from_ddiv_pv` (bool), `n_expiries_implied`, `n_expiries_published`, `max_forward_gap_rel` (= `max_implied_vs_published_forward_rel`), `implied_carry_seconds` (loader / state accounting above) |
| `start.state_seconds`, `.evaluate_seconds` | number | first bucket's state build, `RiskStream::start` evaluate |
| `steps[].bucket_from`, `.bucket_to` | string | the step |
| `steps[].ts_from`, `.ts_to` | int64 | epoch ns of the two states |
| `steps[].dt_years` | number | explain dt (base state's TimeSpec) |
| `steps[].clocks.{load,state,evaluate,family_build,explain,step}_seconds` | number | stage clocks (above) |
| `steps[].frame.{families_built,carry_sets_built,family_rows,cold_rows,exercised_rows,refused_rows}` | uint | shifted frame's `FrameCounters` |
| `steps[].frame.family_nodes` | uint | sigma nodes solved over the families built (`FrameCounters::family_nodes`; nodes per family = family_nodes / families_built) |
| `steps[].frame.routes` | object | unique options per `RiskRoute` name (None … Refused) |
| `steps[].frame.flags` | object | unique options carrying each `RiskFlag` bit, by name |
| `steps[].explain.n_legs`, `.n_ok`, `.n_excluded` | uint | legs (options then shares), Ok, excluded |
| `steps[].explain.excluded_premium` | number | Σ base premium of excluded legs |
| `steps[].explain.excluded_by_reason` | object | excluded legs per `RiskLaneStatus` name |
| `steps[].explain.pnl_total`, `.explained` | number | totals; explained = pnl_total − pnl_unexplained |
| `steps[].explain.sum_abs_unexplained`, `.sum_abs_pnl_total` | number | Σ over Ok legs |
| `steps[].explain.explained_ratio` | number | sum_abs_unexplained / sum_abs_pnl_total |
| `steps[].explain.axes` | object | totals per axis: `delta gamma vega volga vanna theta rho charm unexplained vol_level vol_skew vol_curv vol_resid shares` |
| `steps[].explain.axis_share` | object | each axis / (Σ abs of delta … charm + unexplained) |
| `steps[].quality.spearman`, `.ks`, `.zone` | number, number, string | per-leg explained vs actual; zone `green`/`amber`/`red`/`undefined` (diagnostic) |
| `steps[].vendor_parity.{price,delta,vega}` | object | `n`, `mae`, `sym_rel`, `max_abs` |
| `steps[].determinism.checked`, `.bit_equal` | bool | `--determinism` result |
| `steps[].cold` | object | `ran`, `n_compared`, `evaluate_seconds`, `speedup` (cold / main evaluate), `max_abs_{price,delta,gamma,vega,theta,vanna,volga,charm}` (unique options Ok in both; `null` when none), `max_abs_pnl_total` (legs Ok in both; `null` when none), `max_rel_price_family` (the family price gate: max \|Δprice\| / \|cold price\| over the unique options Ok in both that the main frame served from a family and whose cold price is nonzero; `null` when none) |
| `summary.{load,state,evaluate,explain,step}` | object | `p50`, `p95` (nearest rank over steps), `mean`, `total` |
| `summary.contracts_per_second` | number | n_unique_options / evaluate p50 |
| `summary.legs_per_second` | number | (option + share legs) / explain p50 |
| `summary.run_seconds_excl_io` | number | start state + start evaluate + Σ (state + step); parquet load excluded |
| `summary.explained_ratio` | number | over all steps |
| `summary.spearman_steps`, `.ks_steps`, `.zone_steps` | number, number, string | the per-step totals series |
| `summary.cold_lane_fraction` | number | Σ cold_rows / Σ (family_rows + cold_rows) |
| `summary.determinism_checked`, `.determinism_bit_equal` | bool | over all steps |
| `summary.cold_speedup_p50` | number | median cold speedup (`--kernel both`) |

The markdown summary (`--md`) restates the inputs, the clock table (ms per step), the
throughput, explain quality and determinism lines, one row per step, and one line per
step with the shifted frame's counters (`families_built`, `carry_sets_built`,
`family_rows`, `cold_rows`, `exercised_rows`, `refused_rows`), its nonzero route counts
and its nonzero flag counts (`flags none` when no flag is set), e.g.
`- 1000->1030: families_built 1600, carry_sets_built 0, family_rows 9800, cold_rows 76,
exercised_rows 0, refused_rows 0; routes Family 9800, ColdAnalytic 76; flags
VendorParityAvailable 9876`.

## Per-leg dump (`--dump-legs`)

Per-contract visibility for vendor parity: `--dump-legs legs.csv` writes one row per
OPTION leg (share legs are not dumped) for the first `--dump-steps` steps (default 1;
0 = every step), e.g.

```powershell
atx-vol-risk-bench --store C:\atx-data\spiderrock-30min --from 1000 --to 1500 --kernel family `
                   --threads 8 --out run.json --dump-legs legs-1000.csv --dump-steps 1
```

The file is opened (header written) before any bucket is read, so a bad path fails
fast (`IoError`, exit 1). Rows are in PREPARED row order (grouped by uid, expiry, side;
ascending strike), ties by leg input index. Header row, LF line endings, doubles in
shortest round-trip form, a non-finite value is an EMPTY cell, a text cell holding
`,` `"` CR or LF is quoted (RFC 4180). Every model column is the step's **base** side:
the base frame (`RiskStream::previous()` after the step), the base state's carry and the
base bucket's raw row; the P&L columns are the step's per-leg explain. (The report's
`vendor_parity` uses the shifted frame, so the first step's dump and its parity line
are one bucket apart.)

| columns | source |
|---|---|
| `step` | `<from>-><to>` |
| `leg_index`, `leg_id`, `prepared_row` | option-leg input index, the leg's id, its prepared row |
| `ticker`, `uid`, `uid_slot` | underlier; `uid_slot` indexes `PreparedRiskBook::uids()` |
| `strike`, `expiry`, `side` | contract; `expiry` is the expiry instant's UTC date `YYYY-MM-DD`; `call` / `put` |
| `qty`, `multiplier` | signed contracts; the effective deliverable (≤ 0 / non-finite ⇒ 100) |
| `T`, `sigma`, `forward`, `rate`, `q_eff` | the base frame's resolved inputs (what the kernel priced at) |
| `spot` | the base state's `UnderlierState::spot` (median uPrc of the bucket) |
| `carry_source` | `term_carry` (the uid's `ExpiryCarry` row with the exact expiry), `flat` (no row: flat_rate / flat_q, no cash dividends), `flat_curve_or_divs` (no row; a yield curve or cash dividends apply: carry_rate = flat_rate when no yield curve applies (empty under a curve); pv_divs empty), `surface`, `absent` |
| `carry_rate`, `carry_q`, `pv_divs` | that row's rate / q / pv_divs (flat: flat_rate, flat_q, 0) |
| `q_eff_d4` | `spiderrock_q_eff(spot, pv_divs, carry_rate, carry_q, T)`: the carry the row implies; compare with `q_eff` |
| `carry_mode` | the basis of the contract's (uid, expiry) row: `implied_from_calls` / `published` (empty when the state wrote no row) |
| `implied_forward` | the median call-implied F of that expiry (set even when the guards rejected it; empty when no call solved or under `Published`) |
| `published_forward` | the Published row's (spot − pv_divs)·e^{(rate − q)·T} on the state's clock |
| `route`, `flags`, `status` | the base frame row: `to_string(RiskRoute)`, the `to_string(RiskFlag)` names of its set bits joined by `\|`, `to_string(RiskLaneStatus)` |
| `leg_status` | the explain's status for the leg (Ok or the exclusion reason) |
| `price`, `delta`, `gamma`, `vega`, `theta`, `vanna`, `volga`, `charm` | ours, base frame, per share (`risk_frame.hpp` conventions: vega per unit vol, theta per calendar year) |
| `vendor_price`, `vendor_delta` | the vendor as the engine carried it (the marks; empty unless `VendorParityAvailable`) |
| `vendor_vega`, `vendor_theta` | `sr_ve / 0.01` (per unit vol, what parity compares), `sr_th × 252` (per year) |
| `price_abs_err`, `price_sym_rel` | \|price − vendor_price\| and that over max(\|price\|, \|vendor_price\|, 0.01) |
| `sr_prc`, `sr_de`, `sr_ga`, `sr_ve`, `sr_th` | the base bucket's raw srPrc, de, ga, ve, th (SpiderRock units) |
| `sr_years`, `sr_uprc`, `sr_synspot`, `sr_rate`, `sr_sdiv`, `sr_ddiv`, `sr_bid`, `sr_ask` | the same row's years, uPrc, synSpot (empty when the store has none), rate, sdiv, ddiv, bidPrc, askPrc (raw) |
| `pnl_total`, `pnl_delta`, `pnl_gamma`, `pnl_vega`, `pnl_volga`, `pnl_vanna`, `pnl_theta`, `pnl_rho`, `pnl_charm`, `pnl_unexplained` | the step's per-leg explain (position-scaled) |

Reading it: a carry gap shows as `q_eff` ≠ `q_eff_d4`, as `implied_forward` far from
`published_forward`, or as `sr_uprc` / `sr_years` far from `spot` / `T` on the rows with
the large `price_abs_err`; a kernel defect shows as a large error with matching inputs.
The writer is engine-free and pinned by `RiskBenchReport.LegDumpCsvFormat`; the driver
half (frame → column mapping, carry precedence, book mapping, an unwritable path) by
`RiskBenchE2E.SyntheticRunProducesReport`.

## Tests and integration

- `SpiderRockStore.*`, `SyntheticStore.*`, `BucketState.*`
  (`tests/spiderrock_store_test.cpp`) and `RiskBenchArgs.*`, `RiskBenchBook.*`,
  `RiskBenchStats.*`, `RiskBenchReport.*` (`tests/risk_bench_test.cpp`) link without the
  engine (risk slice). `SpiderRockStoreReal.LoadsBucket1000` and
  `RiskBenchBookReal.SelectsFullBookOnBucket1000` run only with
  `ATX_VOL_SPIDERROCK_STORE` set (GTEST_SKIP otherwise);
  `SyntheticStore.DISABLED_FullSizeGenerationTiming` is an on-demand timing receipt.
  Scratch stores live under the process's private temp root (atx-test-scratch) with a
  pid suffix, so parallel ctest processes and pool trees never share one.
- `RiskBenchE2E.SyntheticRunProducesReport` (`tests/risk_bench_e2e_test.cpp`) and
  `tools/risk_bench.cpp` join `atx-vol-tests` only under `-DATX_VOL_RISK_E2E=ON`
  (default OFF) because they link the risk engine (present on the integration branch:
  `configure -Preset vol-rel -DATX_VOL_RISK_E2E=ON`, then build `atx-vol-risk-slice`).
  With `--dump-legs` on every step it checks the CSV: 1 + 2 × 400 LF lines under the
  header, every row `term_carry` with |q_eff − q_eff_d4| ≤ 1e-12, `carry_mode`
  `published`, `published_forward` = `forward` (1e-12), each row's contract = its book
  leg's, prepared order, and that a directory as `--dump-legs` is an `IoError`.
- Loader / state conventions are versioned by `kSpiderRockConventionsVersion`
  (`spiderrock_store.hpp`; 2 = this document's conventions): persistence fingerprints of
  cached market states depend on it, so any change to a value derived from the vendor
  columns (clock, carry mode, framework filter, pv_divs source) bumps it.
