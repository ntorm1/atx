# Design — Single-File ORATS History Loader → End-to-End US-Equity Smoke (p3 S3)

**Date:** 2026-06-16
**Status:** approved (design); pending implementation plan
**Module:** `p3` (atx-engine v4 — Real-Data Validation & Robustness Verification)
**Sprint:** S3 — Single-File ORATS History Loader (new; feeds the still-unopened S2 benchmark)
**Author:** brainstormed with the user 2026-06-16

---

## 1. Problem & motivation

p3 S1 (Real-Data Hardening, merged) built a real-data path by joining **two** on-disk
datasets — databento daily OHLCV ⋈ a separately-built security master (Yahoo/SEC
corporate actions + fundamentals). That path works but carries three frictions:

1. **Two-source join.** Price (databento, from 2024-07) and corporate actions
   (security-master crawl) are independent datasets that must be axis-aligned by symbol
   and date; coverage is the *intersection* (3-symbol committed smoke; 500-name liquid
   crawl for S2).
2. **Listed-only survivorship bias.** The security master is built from Nasdaq-Trader
   *active* symbols, so delisted names are silently absent — a bias S1 had to *document*
   rather than fix.
3. **Short history.** databento coverage starts 2024-07; barely over a year of price
   data to benchmark on.

The user supplied a single self-contained file — `tbltickerhistory3_10y.txt` (11 GB,
TSV, 71 columns, ~10 years) — that resolves all three. It is an ORATS SMV
`tbltickerhistory` export carrying, in **one row per (date, security)**:

- **Point-in-time symbology** — `securityID` (stable integer), `ticker_tk` (as-traded
  ticker), `todayTicker` (current ticker). A renamed/merged entity keeps one
  `securityID` across the rename (e.g. `AA`→`HWM`, Alcoa→Howmet), and delisted-then-
  reused tickers stay distinct — so the file is **far less survivorship-biased** than the
  listed-only master.
- **Prices** — `open`/`high`/`low`/`close`, plus `closePr` and `closeUnadjPr`
  (unadjusted close).
- **Size / volume** — `volume`, `shares` (shares outstanding).
- **Corporate actions** — `returnFactor` (daily total-return step), `totalReturn`,
  `cumulReturnFactor` (cumulative total-return back-adjustment factor — splits **and**
  dividends folded in).
- **Classification** — `GICS` (sector code), `earnFlag`.
- **Implied/historical-vol term structure** — ORATS `atmCenI_*` / `atmCenH_*` /
  `nEarnCnt_*` columns (an option-implied vol surface summary per name per day).

Because one file carries price + corporate actions + a universe (shares, GICS), it can
seed a **full end-to-end smoke** of the unchanged p2 mine→validate→robust pipeline on the
US equity universe with **no second dataset and no join**.

**Verified mechanic (the corporate-action key):** `close × cumulReturnFactor`
equals the present-basis split+dividend-adjusted close. AAPL 2012-03-26:
`606.98 × 0.0299354 = 18.16`, matching Yahoo's adjusted AAPL for that date (~$18.5).
`cumulReturnFactor` is a step function — constant between ex-dates, stepping only at
corporate-action events — directly analogous to S1's `cumulative_adjustment_factor`, but
**total-return** (dividends included) rather than split-only. (`closeUnadjPr` is **not**
the adjustment input: inspection shows row *t*'s `closeUnadjPr` equals row *t-1*'s `close`
— it is a *lagged prior-day close* helper column. The as-traded contemporaneous close is
`close` (col 9); it is ingested as a raw field but the canonical adjusted price is
`close × cumulReturnFactor`.)

---

## 2. Goals / non-goals

### Goals
- A **pure, high-performance C++ loader** that streams the zip (no full-file
  decompression to disk or 11 GB heap) and writes an **on-disk atx-tsdb partition**
  (per-date sealed `.seg` hive), keyed on `securityID`.
- **Wire the partition into the atx-engine data interface**, expanding that interface
  where the current single-segment attach is insufficient (a **multi-segment** Panel
  attach spanning a date window).
- **Take corporate actions into account via `cumulReturnFactor`** — canonical adjusted
  `close` = total-return index, raw retained.
- A **full end-to-end smoke** on the US equity universe: real zip → partition → assembled
  `alpha::Panel` → the **unchanged** p2 pipeline, deterministic and digest-pinned.
- Reuse the S1 universe / digest / Catalog / adjustment machinery rather than
  re-implementing it.

### Non-goals (this sprint)
- **No options/IV-surface alpha capability.** The `atmCenI_*` columns are *ingested* (a
  curated subset, below) but no new IV-based search operator or objective is added — that
  is p4+. (Consistent with the p3 anti-roadmap: no new alpha-discovery capability.)
- **No change to the segment binary format** (`atx-tsdb` v2) or the S6 `Dataset`/`Catalog`
  API — S3 is a *consumer/producer* on the existing formats.
- **No dividend cash-flow engine** — total-return adjustment only (same decision as S1).
- **No benchmark scorecard** — that is S2. S3 delivers the *data foundation + smoke*; S2
  consumes it.
- **No live execution.** Unchanged from p2.

---

## 3. Field set baked into the partition

Per the user's explicit constraints (post-2020 only; curated IV/earnings columns), the
loader projects **~21 of the 71** columns. Forward-looking columns (`hEMove`, `iEMove`,
`shD1`, `lnD1`, `wkD1`, `qtrD1`, the `*EMove` family) are **excluded** — they encode
expected/realized moves over a *forward* horizon and risk look-ahead leakage into a
backtest Panel.

| Group | Columns kept | Role in the segment |
|---|---|---|
| Identity | `securityID` (key), `ticker_tk`, `todayTicker` | `securityID` → segment symbol name; tickers → side-car symbology manifest |
| Date | `tradingDate` (**filter ≥ 2020-01-01**) | per-date partition key / time axis |
| Price | `open`, `high`, `low`, `close`, `closePr`, `closeUnadjPr` | raw price field blocks (TRI derived downstream) |
| Size/Volume | `volume`, `shares` | volume field; `shares` → market cap in universe |
| Corp action | `returnFactor`, `totalReturn`, `cumulReturnFactor` | `cumulReturnFactor` → TRI adjustment |
| Classification | `GICS`, `earnFlag` | sector code; earnings flag |
| IV term structure | `atmCenI_21d`, `atmCenI_126d` | short + long ATM implied-vol (curated, ingest-only) |
| Earnings | `nEarnCnt_5d` | earnings-count feature (curated, ingest-only) |

Field-block values are `f64`; string identity (`securityID`) becomes the segment symbol
name, and the human tickers (`ticker_tk`, `todayTicker`) live in a side-car symbology
manifest (the segment format stores one symbol name per instrument and `f64`-only field
blocks, so a string ticker cannot be a field block).

---

## 4. Architecture

### 4.1 Data flow

```
tbltickerhistory3_10y.zip   (single DEFLATE entry: tbltickerhistory3_10y.txt, 11 GB)
  │
  ▼  [S3-1] streaming inflate (miniz mz_zip_reader_extract_iter_*) — no 11GB heap
  │        chunked TSV splitter (memchr) + fast field parse (std::from_chars / fast_float)
  │        column projection (~21/71) · tradingDate ≥ 2020-01-01 filter · header schema-guard
  ▼
LongColumns, grouped date-major
  │
  ▼  [S3-2] build_from_long → per-date sealed .seg
  │        data/orats_history_1d/date=YYYY-MM-DD.seg   (symbol name = securityID)
  │        side-cars: _symbology.parquet (securityID→ticker_tk/todayTicker)
  │                   _manifest.json     (row/date/symbol counts, field list, source digest)
  ▼
on-disk atx-tsdb partition
  │
  ▼  [S3-4] attach_multi_segment_panel(seg_dir, window, fields, universe)   ← INTERFACE EXPANSION
  │        union per-date securityID axes → ONE borrowed alpha::Panel over [start,end)
  ▼
  ▼  [S3-5] build_history_panel(HistoryDataConfig)   (mirrors build_real_panel)
  │        canonical close = TRI = close × cumulReturnFactor (S3-3); raw close retained
  │        universe screen (reuse build_universe: mktcap = shares × raw close, ADV, GICS, mask)
  │        DatasetCatalog + lineage · GOLDEN DIGEST PIN
  ▼
assembled alpha::Panel (drop-in)
  │
  ▼  [S3-6] UNCHANGED p2 mine→validate→robust pipeline (RobustResearchDriver) + reference alpha
  ▼
full US-equity end-to-end smoke (deterministic, time-budgeted)
```

### 4.2 Units (one clear purpose each)

- **S3-1 `external/orats_history.{hpp,cpp}` — streaming zip→TSV reader.**
  *Does:* streams the single zip entry through miniz's iterative extractor, splits lines
  on `\n`, splits fields on `\t`, parses only the projected column indices into typed
  buffers, applies the `tradingDate ≥ 2020-01-01` filter, and validates the header against
  the expected schema (fail-closed `Err` on drift). *Depends on:* `atx_miniz`,
  `atx::core` (`from_chars`/fast_float, `Result`). *Interface:* a pull/callback API that
  yields parsed rows (or column batches) so S3-2 can consume without buffering the file.

- **S3-2 — per-date segment writer + manifests.**
  *Does:* groups the date-major row stream into one date's `LongColumns`, calls
  `atx::tsdb::build_from_long` to write `date=YYYY-MM-DD.seg`, and asserts the date axis is
  monotonically non-decreasing (a regression fails closed; a small reorder buffer is the
  fallback if the file is not perfectly sorted). Emits the symbology + build manifests.
  *Depends on:* `atx::tsdb::build_from_long`, `atx::core::io::parquet_writer` (symbology).

- **S3-3 — TRI corporate-action field.**
  *Does:* a pure transform producing canonical `close` = `close × cumulReturnFactor`
  per (date, security), retaining raw `close` as a separate field. Implemented by reusing
  S1 `adjust_total_return(close, cumulReturnFactor, 0)` (its `total_return_index` equals
  `close × cumulReturnFactor`), inheriting the return-invariance contract and NaN policy (a
  NaN raw close or NaN factor → NaN, never zero-filled; re-anchor at next valid). Validated
  against the AAPL 4:1 split (2020-08-31, in-window) for ex-date continuity and Yahoo
  adjusted-close spot-checks. **Computed in the orchestrator (S3-5), not baked into the
  segment** — the
  segment stays a faithful raw mirror; adjustment is a downstream pure transform (mirrors
  S1, where adjust sits downstream of the raw price Dataset).

- **S3-4 `alpha/segment_panel.{hpp,cpp}` — `attach_multi_segment_panel`.**
  *Does:* the interface expansion. `attach_segment_panel` maps a *single* segment; this
  spans a directory of per-date `.seg` over a `TimeWindow`, unioning the per-date
  `securityID` instrument axes (join by symbol name) into one borrowed `alpha::Panel`.
  Reuses `SegmentReader` and honours the no-look-ahead window cutoff. *Depends on:*
  `atx::tsdb::SegmentReader`, existing `attach_segment_panel` internals.

- **S3-5 `data/history_panel.{hpp,cpp}` — `build_history_panel` orchestrator.**
  *Does:* mirrors `build_real_panel` (`data/real_panel.hpp`): partition → multi-segment
  attach → TRI adjust (S3-3) → universe screen (reuse `build_universe`: market cap =
  `shares × raw close`, trailing ADV, GICS `sector_code`, `in_universe` mask) → register
  `price`/`corp_actions`/`universe` in a `DatasetCatalog` with lineage → assemble the Panel
  in a fixed canonical field order → **golden digest pin** (`signal_set_digest`). Returns a
  `HistoryPanel { panel, digest, lineage }`. *Depends on:* S3-4, S3-3, S1
  `build_universe`, S6 `DatasetCatalog`.

- **S3-6 — full end-to-end smoke.**
  *Does:* runs the actual loader on the real zip → partition → `build_history_panel` over
  the US universe (post-2020 window, liquid screen) → feeds the **unchanged** p2
  `RobustResearchDriver` plus a reference momentum alpha. Deterministic, time-budgeted;
  asserts the pipeline runs clean and the reference alpha's sign is sane. This is the
  "full smoke test on end-to-end pipeline on the US equity universe."

### 4.3 The one architectural decision resolved during design

**Panel rail = per-date partition + `attach_multi_segment_panel`** (chosen over a single
window-segment, and over building both). Rationale: honours the "on-disk atx-tsdb
**partition**" directive (per-date `.seg` is the established hive format), provides the
"expand the interface where needed" deliverable (the multi-segment attach), stays
live-feed-compatible (`MultiSegmentBarFeed` consumes the same per-date `.seg`), and reuses
the entire S1 universe/digest/Catalog machinery. Cost accepted: the cross-segment
`securityID` union is new code (the single-segment alternative would have done that union
once at build time and reused `attach_segment_panel` unchanged, but is not a "partition",
cannot append incrementally, and loses live-feed reuse).

---

## 5. Corporate-action model (locked)

- Segment stores **raw** price fields faithfully (`open`/`high`/`low`/`close`/`closePr`/
  `closeUnadjPr`) plus `returnFactor`/`totalReturn`/`cumulReturnFactor`.
- The orchestrator computes canonical `close` = total-return index =
  `close × cumulReturnFactor` (back-adjusted to present basis, splits **and** dividends
  folded), and **retains** raw `close` as a separate Panel field (`raw_close`).
- **Implemented by reusing S1-3 `adjust_total_return` verbatim:**
  `adjust_total_return(raw_close = close, cum_adj_factor = cumulReturnFactor,
  cash_dividend = 0)` → its `total_return_index` equals `close × cumulReturnFactor`
  exactly (with `D = 0` the geometric chain collapses to `S_t = close × cumulReturnFactor`),
  and it carries the proven **return-invariance ⇒ non-leak** argument and NaN policy for
  free. Because `cumulReturnFactor` already folds dividends, S3 needs **no** per-symbol
  dividend reinvestment loop (S1-3 reconstructed the TRI from split factor + cash dividend;
  here the cumulative total-return factor is given directly).
- Validation: AAPL 4:1 split (2020-08-31) ex-date continuity in the TRI; Yahoo
  adjusted-close spot-checks on 2–3 names.

**Leak guard.** Price back-adjustment is return-invariant ⇒ non-leaking (S1 §0.7 #2). The
file's `shares`/`GICS` are PIT as published in the ORATS row (one value per (date,
security)); S3 consumes them as-of the row date, matching the file's own PIT semantics. (A
distinct first-print-vs-revised bitemporal restatement guard is out of scope, as in S1.)

---

## 6. Testing strategy (TDD per unit)

- **S3-1 / S3-2:** a tiny synthetic zip fixture (≈10 rows, 2 dates, 3 securityIDs,
  full 71-column header) committed to the repo. Assert: projected/filtered columns
  correct; `tradingDate < 2020` rows dropped; sealed `.seg` round-trips through
  `SegmentReader` (values, present-bitmap, symbol names = securityIDs); a corrupted/drifted
  header → `Err`.
- **S3-3:** AAPL 2020-08-31 4:1 split — TRI has no discontinuity at the ex-date;
  return-invariance bit-identical under a constant price rescale (reuse the S1
  `ReturnInvariantUnderConstantPriceRescale` pattern); NaN raw close / NaN factor does not
  zero-fill.
- **S3-4:** three per-date `.seg` fixtures with disjoint + overlapping securityID sets →
  correct axis union, correct cell placement, no-look-ahead window cutoff (truncating
  future dates leaves a past date's cells byte-identical).
- **S3-5:** golden digest pin over a fixed small **real** window, asserted byte-identical
  on a second build (the S1-5 determinism pattern).
- **S3-6:** deterministic, time-boxed E2E; asserts the unchanged pipeline runs clean and a
  reference momentum alpha's sign is economically sane.

The committed C++ tests run against the **tiny synthetic fixture**; the 11 GB build and the
full-universe smoke are an operator data-build (gitignored `data/`), recorded in the
ledger like the S1-6 crawl.

---

## 7. Performance (the "pure high performance" requirement)

- **Streaming inflate** via miniz `mz_zip_reader_extract_iter_new`/`_read` — never
  materialize the 11 GB plaintext (post-2020 filter parses ≈ 45% of rows).
- **Fast parse** — `memchr` to find `\t`/`\n` boundaries; `std::from_chars` / fast_float
  for `f64`; parse only projected column indices (skip the other ~50 fields without
  conversion; a cheap `tradingDate` pre-check can skip an entire pre-2020 row before
  parsing its numerics).
- **Producer/consumer overlap** — an inflate thread feeds a parse/build thread over an
  atx-core SPSC ring (`concurrent/spsc_queue.hpp`), overlapping decompression with parsing.
- **Bounded memory** — only one trading date's rows are buffered at a time (date-major
  input; monotonic-date assertion), so peak memory is ~one date's worth, not the file.
- **Deferred lever:** chunk-parallel parsing (multiple parse workers with line-boundary
  stitching across chunk seams) — not in the first cut; single parse thread first, measured
  before adding complexity.
- **Target:** full post-2020 ingest of the real file in minutes on one machine.

---

## 8. Roadmap placement

New **p3 S3**, sitting after S1 (closed) and feeding the still-unopened **S2** benchmark.
S3 supersedes the databento ⋈ security-master smoke as the data foundation for S2: S2's
benchmark harness (`build_real_panel` today) can switch to `build_history_panel` over the
ORATS partition for a longer-history, less-survivorship-biased, single-source universe.
S3 adds **no** new alpha-discovery capability (consistent with the p3 anti-roadmap); it is
a data-ingestion + interface-expansion + smoke sprint.

The ROADMAP's sprint arc gains an S3 box; the S2 outline is annotated to note it may
consume the S3 partition. The `data-ingestion-reference.md` gains a section for the ORATS
single-file source alongside the databento + security-master sections.

---

## 9. Open defaults (state-and-proceed)

- **Partition root:** `data/orats_history_1d/` (gitignored, like other `data/` builds).
  Committed C++ fixture = the tiny synthetic zip, not the 11 GB build.
- **Smoke universe screen:** reuse S1 `UniverseConfig` defaults (`adv_window = 21`,
  `min_adv_usd = 1e6`, `min_mktcap_usd = 0`, `top_n_by_adv = 0`); the final S2 benchmark
  thresholds + universe size stay an S2 fork.
- **GICS encoding:** the file's `GICS` is a single-digit code (an ORATS sector encoding,
  not the 2-digit GICS sector code); map it to `sector_code` and document the encoding in
  S3-3 (sentinel `kNoSectorCode = -1` for missing, never `0`, matching S1-4).
- **Window for the committed E2E digest pin:** a fixed small post-2020 window
  (e.g. `[2020-01-02, 2020-02-01)`) over a handful of securityIDs in the synthetic
  fixture; the real-data full-universe run is the operator data-build.

---

## 10. Exit criteria

1. A single documented call (`build_history_panel`) assembles a **byte-reproducible**
   (`digest`-pinned) `alpha::Panel` from the on-disk ORATS partition, with corporate
   actions applied via `cumulReturnFactor` (TRI canonical close, raw retained) and a
   market-cap/ADV/sector-screened universe.
2. The **pure C++ loader** streams the real 11 GB zip into a per-date `.seg` partition
   keyed on `securityID`, with the post-2020 filter and curated field set, in bounded
   memory and in minutes.
3. The data interface is expanded with `attach_multi_segment_panel`, spanning the
   partition into one Panel over a date window.
4. A **full end-to-end smoke** drives the **unchanged** p2 mine→validate→robust pipeline
   on the assembled US-equity Panel, deterministically and within a time budget.
5. The corporate-action adjustment is validated against the AAPL 2020 split + Yahoo
   spot-checks; the reference alpha's sign is economically sane.
6. `data-ingestion-reference.md` documents the new source; `sprint3.md` + the ledger close
   the sprint; residuals → backlog; `--no-ff` merge.

---

*Sprint discipline per `atx-engine/plans/docs/sprint.md` + `module.md`; implementation
quality per `implementation-quality.md`; C++ governance per `.agents/cpp/agent.md`.*
