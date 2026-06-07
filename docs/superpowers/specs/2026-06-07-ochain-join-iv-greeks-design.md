# OCHAIN Join + IV + Greeks Design

**Date:** 2026-06-07
**Status:** Approved
**Worktree/branch:** `feat/disk-data-layer`

## Goal

Build the `OCHAIN` store by joining OPRA option-chain L1 quotes (`OPRA_BBO`)
onto the underlying equity L1 quotes (`OHLC1M`) for a single date partition,
then enrich each option row with time-to-expiry, mid-price implied volatility,
and Black-Scholes greeks. Output one float64-typed Parquet partition per date.

First target partition: `date=2026-06-05` (324,770 option rows, 100 underlyings).

## Output Schema

`<root>/OCHAIN/date=YYYY-MM-DD/data.parquet`, columns in this order:

| column     | dtype     | source / formula                                  |
|------------|-----------|---------------------------------------------------|
| timestamp  | Timestamp | OPRA `ts` (ns)                                    |
| date       | string    | partition date, physical column ("2026-06-05")   |
| underlying | string    | OPRA `underlying`                                |
| expiry     | string    | OSI expiry → "YYYY-MM-DD"                         |
| call_put   | string    | "C" or "P" from OSI                              |
| strike     | f64       | OSI strike integer / 1000                         |
| obid       | f64       | OPRA `bid_px` × 1e-9 (NaN if unset/≤0)            |
| oask       | f64       | OPRA `ask_px` × 1e-9 (NaN if unset/≤0)            |
| ubid       | f64       | joined OHLC1M `bid_px` × 1e-9                     |
| uask       | f64       | joined OHLC1M `ask_px` × 1e-9                     |
| years      | f64       | ACT/252 trading-day fraction to expiry            |
| mid_iv     | f64       | implied vol from option mid (NaN if unsolvable)   |
| de         | f64       | delta at solved σ (NaN if unsolved)               |
| ga         | f64       | gamma                                            |
| ve         | f64       | vega (per 1.00 vol, per year)                     |
| th         | f64       | theta (per calendar day)                          |

`date` is written as a **physical** string column (not only path-encoded) so the
file is self-describing and readable by `atx::core::io::LazyParquet`, which does
no Hive partition inference. Driver writes via `core::io::write_parquet` directly
to `DiskStore::partition_path(Store::OChain, date)`.

## Input Schemas (verified)

`OPRA_BBO/date=2026-06-05/data.parquet` (324,770 rows):
`ts:Timestamp, underlying:string, symbol:string(OSI 21-char), bid_px:i64,
ask_px:i64, bid_sz:i64, ask_sz:i64` (+ path date). Prices are 1e-9 fixed-point;
undefined price sentinel is `INT64_MIN` (from `px_or_unset`).

`OHLC1M/date=2026-06-05/data.parquet` (100 rows):
`ts:Timestamp, symbol:string(plain ticker, e.g. "BRK.B"), bid_px:i64, ask_px:i64,
bid_sz:i64, ask_sz:i64` (+ path date).

## Join

Key: `OPRA_BBO.underlying` (already dot-stripped OSI root, e.g. "BRKB") joined
against `normalize(OHLC1M.symbol)` where `normalize` strips '.' (BRK.B → BRKB).
Coverage verified 100/100 underlyings. Build an
`unordered_map<string,(ubid_i64,uask_i64)>` from OHLC1M once, then look up per
OPRA row. Rows with no underlying match (none expected) → ubid/uask NaN, IV NaN.

## OSI Symbol Parsing

OSI 21-char layout: `RRRRRRYYMMDDTSSSSSSSS`
- chars 0–5: root, space-padded right (rstrip spaces).
- chars 6–11: expiry `YYMMDD` → year `2000+YY`, month, day.
- char 12: type `C` or `P`.
- chars 13–20: strike, 8 ASCII digits = strike × 1000 → `f64 = atoi / 1000.0`.

Examples: `"AAPL  270617P00250000"` → (AAPL, 2027-06-17, P, 250.0);
`"XLF   260612C00058000"` → (XLF, 2026-06-12, C, 58.0).

Unit: `atx/engine/quant/osi.hpp` (header-only, pure).
```cpp
struct OsiOption { std::string root; int y, m, d; bool is_call; double strike; };
[[nodiscard]] std::optional<OsiOption> parse_osi(std::string_view sym);
```

## Time to Expiry — ACT/252

Snapshot is 19:55 ET (after the 16:00 ET close), so the observation day
contributes no remaining time. Define:

```
years = trading_days_in (obs_date, expiry_date]  / 252.0
```
i.e. count NYSE trading days `d` with `obs_date < d <= expiry_date`. Same-day
expiry → 0 trading days → `years = 0` → IV/greeks NaN.

Unit: `atx/engine/quant/trading_calendar.{hpp,cpp}`.
- Civil-date ↔ days-from-epoch via Howard Hinnant's `days_from_civil` /
  weekday algorithms (integer, branch-light, no `<chrono>` calendar dependency).
- Embedded NYSE holiday set for **2026, 2027, 2028** (covers max expiry
  2028-12-15): New Year's Day (observed), MLK Day, Washington's Birthday,
  Good Friday, Memorial Day, Juneteenth, Independence Day (observed), Labor Day,
  Thanksgiving, Christmas (observed). Weekend-adjustment rule applied for
  fixed-date holidays (Sat→Fri, Sun→Mon). Early-close days are full trading days
  for day-count purposes, so ignored.
- API:
```cpp
[[nodiscard]] bool is_trading_day(int y, int m, int d);
[[nodiscard]] int trading_days_between(int y0,int m0,int d0, int y1,int m1,int d1);
   // count trading days strictly after (y0,m0,d0), up to and including (y1,m1,d1);
   // returns 0 if to-date <= from-date.
[[nodiscard]] double act252_years(int y0,int m0,int d0, int y1,int m1,int d1);
   // trading_days_between / 252.0
```

## Black-Scholes Model

Constants: `r = 0.043`, `q = 0.0` (hardcoded in driver, passed into functions).
`S = (ubid+uask)/2`, option `mid = (obid+oask)/2`.

Unit: `atx/engine/quant/black_scholes.hpp` (header-only, pure, `inline`).
```cpp
[[nodiscard]] double norm_cdf(double x);   // 0.5*erfc(-x/sqrt2)
[[nodiscard]] double norm_pdf(double x);
[[nodiscard]] double bs_price(double S,double K,double T,double r,double q,
                              double sigma,bool is_call);
[[nodiscard]] double bs_vega(double S,double K,double T,double r,double q,double sigma);
struct Greeks { double delta, gamma, vega, theta; };  // theta per calendar day
[[nodiscard]] Greeks bs_greeks(double S,double K,double T,double r,double q,
                               double sigma,bool is_call);
[[nodiscard]] double implied_vol(double price,double S,double K,double T,
                                 double r,double q,bool is_call);
```

`implied_vol`: returns `NaN` when any input is non-finite, `T<=0`, `S<=0`,
`price<=0`, or `price` violates no-arbitrage intrinsic/upper bounds. Otherwise
Newton-Raphson seeded at σ=0.5 using `bs_vega`; if a step leaves
[1e-6, 5.0] or vega underflows, fall back to bisection on [1e-6, 5.0].
Converged when `|price_model - price| < 1e-7` or 100 iterations. Non-convergence
→ NaN.

`bs_greeks`: `delta` = e^{-qT}N(d1) (call) / e^{-qT}(N(d1)-1) (put);
`gamma` = e^{-qT}φ(d1)/(Sσ√T); `vega` = S e^{-qT}φ(d1)√T (per 1.0 vol, per year);
`theta` = full BS theta **/ 365** (per calendar day).

## Driver — Parallel Join + Compute

Unit: `atx-engine/examples/build_ochain.cpp`. Usage:
`build_ochain [DATA_ROOT] [DATE]` (defaults `data/universe`, `2026-06-05`).

1. `DiskStore::open(root)`.
2. Scan `OHLC1M` partition → build `unordered_map<string,pair<i64,i64>>`
   keyed by dot-stripped symbol → (bid_px, ask_px).
3. Scan `OPRA_BBO` partition → materialize columns
   (ts, underlying, symbol, bid_px, ask_px) via `ParquetTable::column_view`/`strings`.
4. Allocate output vectors sized to row count.
5. **Parallel fan-out:** split `[0,N)` into `hardware_concurrency()` contiguous
   chunks; one `std::thread` per chunk. Each row independently: parse OSI, look up
   underlying, compute `years`, mids, `implied_vol`, `bs_greeks`; write results to
   its own output indices. No shared mutable state ⇒ deterministic, lock-free.
   (Per-thread reads of the shared underlying map and input spans are const.)
6. Build `WriteColumn` vector (date as a same-length string column) and
   `write_parquet` to `partition_path(Store::OChain, date)`.
7. Print row count, #IV solved, #NaN, elapsed.

Wired into `atx-engine/examples/CMakeLists.txt` as a new executable linking
`atx::engine atx::core`.

## Error Handling

- Missing partition / scan failure → `Result` error, print + exit 1.
- Per-row failures (bad OSI, no underlying, unsolvable IV) never abort the run;
  they yield NaN in the affected output columns. The join is lossless: every
  OPRA_BBO row produces exactly one OCHAIN row.
- Undefined fixed-point prices (`INT64_MIN`) and non-positive prices map to NaN
  floats and disqualify IV.

## Testing

- `osi_test.cpp`: parse standard, padded-root, dot-stripped-root (BRKB),
  call & put, fractional strike (58.5 → "00058500"), reject malformed length.
- `trading_calendar_test.cpp`: weekend exclusion; a known holiday excluded
  (e.g. 2026-07-03 observed Independence Day, 2026-12-25 Christmas); a plain
  week = 5 trading days; same-day → 0; backwards range → 0; ACT/252 fraction.
- `black_scholes_test.cpp`: `bs_price` vs known reference (ATM call), put-call
  parity, `implied_vol` recovers a planted σ within 1e-4, greeks vs central
  finite-difference within tolerance, NaN guards (T=0, price below intrinsic).
- Optional integration: tiny synthetic OHLC1M+OPRA_BBO → run join logic →
  assert column count, row count, and a hand-computed IV/greek for one row.

## Non-Goals

- No American-exercise / early-exercise premium (Black-Scholes European only).
- No dividend schedule (`q=0`).
- No multi-date batch loop (single date per invocation; loop is trivial later).
- No interpolated term-structure risk-free curve (flat `r`).
