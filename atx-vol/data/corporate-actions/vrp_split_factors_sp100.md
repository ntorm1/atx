# SP100 Split Adjustment Factors — `vrp_panel --splits` Reference

## Overview

`vrp_split_factors_sp100.tsv` is the corporate-action reference the VRP panel
consumes via `bev_label_factory --vrp-panel --panel-schema v2 --splits <file>`.
It carries split and reverse-split **price factors** for the SP100 universe
(`atx-vol/data/universe/sp100_2026-07.csv`, 102 symbols) over the full span of
the atx-db warehouse.

`price_factor` multiplies every session **strictly before** `ex_date`, so a
10:1 forward split is `0.1` and a 1:8 reverse split is `8`. That is the same
convention as the warehouse's own factor column, and the same one
`apply_vrp_split_adjustment` (`atx-vol/src/analytics/vrp_panel.hpp`) applies.

## Why it exists

A `SurfaceDb` spot is the raw session spot and steps discontinuously across a
split ex-date. `rv_fwd_21d` reads that step as a genuine return, which produced
the round-1..3 defect this file closes: **56 label rows** with `rv_fwd > 3.0`
(worst **11.15**), a per-symbol `label_sd` spread of **3423x**, and a panel
mean label of **+0.0155** — the wrong sign for a variance risk premium.

## Source

| field | value |
|---|---|
| Warehouse | `C:\atx\atx-db\data\warehouse.duckdb` (opened READ-ONLY) |
| Table | `equity_daily_bars`, column `split_factor` |
| Warehouse span | 31,173,360 bars / 26,069 symbols / 2012-03-26 .. 2026-06-15 |
| Generator | `atx-vol/scripts/vrp_split_factors.py` |
| Vendor lineage | `tbltickerhistory3_10y` |

The vendor factor is **copied through unchanged**. This file contains no
ratio derived from observed prices and no detection threshold that could
invent an event.

## Two filters, both justified against in-repo sources

### 1. Split/dividend band — `price_factor <= 0.8 or >= 1.25`

`equity_daily_bars.split_factor` carries **both** split and cash-dividend
price adjustments in one column (BKNG's quarterly dividends appear as
~0.9977). Dividend factors must not be back-adjusted out of a volatility
panel: a dividend drop is a real price return that realized vol legitimately
contains.

The discriminator is not invented here. It is atx-db's own published
`split_policy` — `included_factors: "split_factor <= 0.8 or >= 1.25"`,
`excluded: "small dividend adjustment factors"` — at
`atx-db/src/atx_db/earnings_acceleration.py:123` and `:284`, with the
identical predicate at `earnings_surprise.py:164`. Reusing it verbatim keeps
the panel and the warehouse's own factor pipelines in agreement by
construction.

### 2. Corroboration — `0.75 <= (prev_close/close) * price_factor <= 1.3333`

The vendor factor column carries **false positives**: a declared factor parked
on the *announcement* date where no price step occurred. The measured case is
V in 2015 — `split_factor = 0.249546` on 2015-02-11 with close 265.99 against
a prior close of 264.55 (no step at all), while the genuine 4:1 ex-date is
2015-03-19 (267.67 -> 66.81, factor 0.25). Emitting the announcement row would
have divided V's entire prior history by four *on top of* the real split.

Each event is therefore cross-checked against a **second, independent column
of the same authoritative source** — the observed close step. This can only
ever *reject* a declared event, never invent one, and the surviving factor is
still the vendor's own number.

Measured over this universe (39 declared events):

| population | n | residual range |
|---|---|---|
| genuine events | 37 | 0.888 .. 1.030 |
| announcement artifact (V 2015-02-11) | 1 | **0.248** |
| no prior bar to check (GOOGL 2014-04-03) | 1 | — |

The band clears the tightest genuine event (TSLA 2020-08-31, 0.8883) by 1.18x
and the artifact by 3.0x, with nothing in between.

Rejected events are excluded from the data rows but listed as `# REJECTED`
provenance lines in the file itself and echoed to stderr — never dropped
silently. An event with no prior bar cannot be checked; it is **emitted** and
flagged `uncorroborated=1`, because refusing a real event is the worse error.

## Why not `adjusted_close`

atx-db's own back-adjuster refuses that column and says why
(`equity_price_metrics.py:_back_adjusted_close`): in the cached sample it is
"an unadjusted lagged close and leaves split jumps in, which would inflate
returns/volatility". The factor column is the authority.

## Contents

38 events emitted (39 declared, 1 rejected, 1 uncorroborated). Columns after
`price_factor` are provenance and are ignored by `load_vrp_split_factors`.

Three events fall inside the round-4 panel window (2025-08-01 .. 2026-07-24)
and are the ones that close the defect:

| symbol | ex_date | ratio | price_factor | raw step | adjusted step |
|---|---|---|---|---|---|
| NFLX | 2025-11-17 | 10:1 | 0.1 | -90.1% | -0.8% |
| NOW | 2025-12-18 | 5:1 | 0.2 | -80.4% | -2.0% |
| BKNG | 2026-04-06 | 25:1 | 0.04 | -95.8% | +5.0% |

The remaining 35 events are historical (2012-2024) and are ignored by
`apply_vrp_split_adjustment` for this corpus, since events at or before a
symbol's first session have no earlier session to scale. They are retained so
the file stays a complete reference as the corpus is backfilled.

## Regenerating

```powershell
# duckdb is required; the atx-db venv already has it.
C:\atx\atx-db\.venv\Scripts\python.exe atx-vol\scripts\vrp_split_factors.py `
  --db C:\atx\atx-db\data\warehouse.duckdb `
  --universe atx-vol\data\universe\sp100_2026-07.csv `
  --out atx-vol\data\corporate-actions\vrp_split_factors_sp100.tsv
```

Output is deterministic — no timestamps, sorted by (symbol, ex_date) — so a
regeneration against an unchanged warehouse reproduces the file byte for byte.

## Known limitations

- **Universe-scoped.** Only SP100 members are scanned. Widening the panel
  universe (round-4 Phase 4, xsec-616) requires regenerating with the wider
  universe file.
- **Spinoffs are approximate.** `T 2022-04-11` (WBD) and `GE 2023-01-04` /
  `2024-04-02` carry residuals 0.933 / 0.954 / 1.023 — a spinoff's price factor
  never explains the step exactly, because the stub's value is not a clean
  multiple. They are accepted as declared; a panel that trades those names
  through those dates should expect a residual return artifact well below the
  `rv_fwd > 3.0` gate but not identically zero.
- **No dividend adjustment.** By design — see the band section.
