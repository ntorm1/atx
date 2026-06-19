# atx::engine::regime — Field Dictionary, Semantics & RUNBOOK

The `regime` module loads offline-staged macro time-series (daily, calendar
dates) and broadcasts them as `regime_<series>` columns into the alpha Panel.
This document is the authoritative field reference for v1.

---

## Field Dictionary

| Column name | Group | Meaning | Source | FRED ID / notes |
|---|---|---|---|---|
| `regime_vix` | vol | CBOE Volatility Index (30-day implied vol of S&P 500 options) | FRED | `VIXCLS` |
| `regime_vvix` | vol | CBOE VVIX (vol-of-vol; 30-day implied vol of VIX options) | CBOE manual | — (see staging note) |
| `regime_move` | vol | ICE BofA MOVE Index (Treasury implied vol) | Yahoo/ICE manual | `^MOVE` on Yahoo Finance |
| `regime_dgs2` | rates/curve | 2-year US Treasury constant-maturity yield (% per annum) | FRED | `DGS2` |
| `regime_dgs10` | rates/curve | 10-year US Treasury constant-maturity yield (% per annum) | FRED | `DGS10` |
| `regime_t10y2y` | rates/curve | 10y–2y Treasury curve slope (DERIVED = dgs10 − dgs2) | computed | derived when both legs present |
| `regime_t3m10y` | rates/curve | 3-month–10-year spread (documented canonical name; **not staged by default**) | FRED | `T10Y3M` — not in loader default |
| `regime_hy_oas` | credit/liquidity | ICE BofA US High Yield OAS (basis points) | FRED | `BAMLH0A0HYM2` |
| `regime_ig_oas` | credit/liquidity | ICE BofA US Corp IG OAS (basis points) | FRED | `BAMLC0A0CM` |
| `regime_nfci` | credit/liquidity | Chicago Fed National Financial Conditions Index | FRED | `NFCI` |
| `regime_spx_dist_200dma` | breadth/trend | SPX distance from 200-day moving average (documented canonical name; **not staged by default**) | manual | not in loader default |

### Canonical name reference

All eleven names above appear in `kCanonicalSeriesArr` in
`atx-engine/include/atx/engine/regime/series.hpp`. The loader
(`atx-impl/src/stage_regime.cpp`) loads whichever of the eight default
series have a CSV present in `--staging-dir`. Series names not in the table
above — `t3m10y` and `spx_dist_200dma` — are defined canonical names that
are **not wired into the v1 loader default**; they can be added to a custom
`RegimeLoadConfig::series` call to extend coverage without a code change.

### Manual staging: vvix and move

Neither CBOE VVIX nor the ICE BofA MOVE Index have a stable, freely
accessible automated CSV endpoint. Stage them by hand:

- **vvix** (`vvix.csv`): download the CBOE VVIX historical CSV from
  <https://www.cboe.com/tradable_products/vix/vix_historical_data/>;
  rename/copy to `<staging-dir>/vvix.csv`. Format expected: `CsvFormat::Cboe`,
  value column `CLOSE`.
- **move** (`move.csv`): download from Yahoo Finance history for ticker
  `^MOVE`; rename/copy to `<staging-dir>/move.csv`. Format expected:
  `CsvFormat::Yahoo`, value column `Adj Close`.

The loader silently skips absent files, so a FRED-only staging dir is valid
— `regime_vvix` and `regime_move` columns are simply omitted from the `.seg`.

---

## Semantics

### Forward-fill (causal)

Each series is forward-filled (last observation carried forward, LOCF) after
loading. This is **causal**: a date `d` receives the most recent observation
at or before `d`. Dates before the first observation in a series are NaN —
they are never back-filled. This matches the behavior of daily data used in
a daily alpha engine where tomorrow cannot know today's close.

### Panel join — as-of lookup

When `with_regime_fields` (or `finalize_panel_with_regime`) appends regime
columns to an alpha Panel, each cell `(date d, instrument i)` is populated by
`store.value(series, panel_dates[d])`. The store's as-of lookup finds the
latest observation at `t <= panel_dates[d]`. Out-of-universe instruments
(where the Panel's universe mask is 0) receive **NaN**, not the macro value,
so regime data does not contaminate masked rows.

### Unknown series

If a series name is requested but was never loaded into the `RegimeStore`,
`store.value` returns NaN for every date — the column exists with all-NaN
values rather than failing. This is a deliberate design: a typo in
`--regime-fields` produces an all-NaN column that alpha expressions evaluate
to zero or NaN, rather than crashing a multi-day backtest.

### Byte-determinism

Running `atx-impl regime` twice against the **same staged snapshot** (same
CSVs, bit-for-bit) produces a **byte-identical `.seg`** file and manifest.
The loader pins `RegimeLoadConfig::created_at_nanos = 0` for this reason
(see `stage_regime.cpp`). Determinism holds per-snapshot: if you re-download
from FRED on a later date you get a different snapshot and a different `.seg`
(FRED revises historical data). Archive the staging dir alongside the `.seg`
to reproduce a specific run.

---

## Usage — regime conditioning in alpha expressions

Regime columns live inside the alpha expression DSL using the existing
comparison and ternary operators. **No new VM opcodes are needed.** A
regime-conditional alpha looks like:

```
regime_vix < 20 ? ts_zscore(-ts_delta(close, 5), 20) : 0
```

This expression evaluates to the momentum signal when VIX is below 20, and
to zero otherwise. In low-vol regimes the alpha is on; in high-vol regimes
it is flat.

### Using a regime-conditional expression as a seed

The `atx-impl discover` subcommand accepts `--seed-expr` (repeatable). Pass
a regime-conditional expression to seed evolutionary discovery with a
regime-aware starting point:

```sh
atx-impl discover \
  --panel data/panel.seg \
  --alpha-out data/alphas/ \
  --seed-expr "regime_vix < 20 ? ts_zscore(-ts_delta(close, 5), 20) : 0" \
  --seed-expr "regime_hy_oas > 400 ? -ts_zscore(close, 20) : ts_zscore(close, 20)" \
  --population 32 --generations 10
```

The panel passed to `discover` must have been built with the regime columns
already broadcast (i.e., `build_real_panel` invoked with `regime_seg_path`
and `regime_fields` set). The DSL parser treats `regime_vix` as a normal
field name — no special handling.

---

## RUNBOOK

### Step 1 — Stage

Download the FRED series to a local staging directory (stdlib only, no deps):

```sh
python scripts/regime/fetch_regime.py data/regime_staging
```

Expected output (one line per series, plus the manual-staging note):

```
staged vix <- FRED VIXCLS
staged dgs2 <- FRED DGS2
staged dgs10 <- FRED DGS10
staged hy_oas <- FRED BAMLH0A0HYM2
staged ig_oas <- FRED BAMLC0A0CM
staged nfci <- FRED NFCI
NOTE: vvix (CBOE) and move (Yahoo/ICE) must be staged manually; ...
```

Optionally copy `vvix.csv` and `move.csv` into `data/regime_staging/`
(see manual staging note above).

### Step 2 — Load

Build the regime `.seg` file from the staged CSVs:

```sh
atx-impl regime \
  --staging-dir data/regime_staging \
  --regime-out  data/regime.seg
```

This writes two files:

- `data/regime.seg` — the sealed, byte-deterministic regime segment
- `data/regime.seg.manifest.json` — series count, date range, stage digest

Optional: `--min-date YYYY-MM-DD` to drop observations before that date.

### Step 3 — Use (broadcast into Panel)

The `RealDataConfig` struct (`atx-engine/include/atx/engine/data/real_panel.hpp`)
accepts `regime_seg_path` and `regime_fields`:

```cpp
RealDataConfig cfg;
cfg.regime_seg_path = "data/regime.seg";
cfg.regime_fields   = {"vix", "t10y2y", "hy_oas"};
auto rp = build_real_panel(cfg);
// rp->panel now contains regime_vix, regime_t10y2y, regime_hy_oas columns.
```

When both fields are set, `build_real_panel` calls `finalize_panel_with_regime`,
which opens the `RegimeStore` and appends one `regime_<series>` column per
requested series, as-of each panel date.

When `regime_seg_path` is empty (or `regime_fields` is empty), the Panel and
digest are **byte-identical to the pre-regime path** — no-regression.

---

## Wiring Honesty Note

**What works today:**

- `atx-impl regime` builds the `.seg` end-to-end (Step 2 above works now).
- `regime::with_regime_fields` and `data::finalize_panel_with_regime` are
  implemented and tested (see `atx-engine/tests/data/data_real_panel_regime_test.cpp`).
- `build_real_panel` is wired to call `finalize_panel_with_regime` when
  `RealDataConfig::regime_seg_path` is non-empty.

**What is not yet wired:**

The `atx-impl panel` subcommand does **not** invoke `build_real_panel` today;
the `--regime-segs` and `--regime-fields` CLI flags are parsed and plumbed
into `RunConfig::regime_segs` / `RunConfig::regime_fields` (see `config.hpp`
and `config.cpp`) but are not consumed by a running `panel` subcommand. The
broadcast path is exercised through `build_real_panel` directly (and its test
suite) but cannot be triggered from the CLI `panel` stage yet.

A reader should not attempt `atx-impl panel --regime-segs data/regime.seg
--regime-fields vix,t10y2y` and expect the regime columns to appear in the
output panel — that wiring is the planned next step after this sprint.
