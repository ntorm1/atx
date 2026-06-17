# Data Ingestion Reference

**A map of the real-data ingestion stack — on-disk parquet → segments → `Dataset`/`Catalog`/`adapt_*` → corporate-action adjustment → universe → the assembled `alpha::Panel`.**

This is a **reference, not a tutorial**: it states *what exists, where (`file:line`), and why*. Every factual claim is anchored to a `file:line` at the current worktree HEAD (`cfcd27b`). The p3 S1 code chain (S1-2 corporate actions, S1-3 adjust, S1-4 universe, S1-5 real-data path) has **landed** — the refs below point at the shipped files. Paths are relative to the repo root. For the alpha pipeline that consumes the Panel this builds, see [`alpha-pipeline-reference.md`](alpha-pipeline-reference.md).

**Source semantics** for the on-disk datasets live in [`docs/us_split_adjustment_factors.md`](../../../docs/us_split_adjustment_factors.md) (the builder's own doc) and the S1 plan §0.5/§0.7 ([`sprint-1-real-data-hardening-implementation-plan.md`](sprint-1-real-data-hardening-implementation-plan.md)). This reference distills them against the as-built loaders; where the two differ, the as-built code wins.

---

## 1. The on-disk datasets (§0.5 schemas + paths)

Two consumed datasets, both gitignored and resolved from the repo root at runtime.

### 1.1 Databento daily OHLCV — the price axis

```
data/databento/equs_ohlcv_1d_by_date/date=YYYY-MM-DD/part-00000.parquet
  schema: date(date32), symbol(str), instrument_id(u32), ts_event(ts[ns,UTC]),
          open/high/low/close(f64), volume(u64), publisher_id, rtype, source_file
  coverage: from 2024-07-01; broad US-equity universe; UNADJUSTED prices.
```

The hive is partitioned one directory per trading date; each partition holds a single `part-*.parquet`. The columns S1-5 reads are `symbol` + `open/high/low/close` (f64 dollars) + `volume` (u64); the `DateKey` is parsed from the partition directory name, not a column (`real_panel.cpp` `window_partitions`, [`src/data/real_panel.cpp:122`](../../src/data/real_panel.cpp)).

### 1.2 Security master — corporate actions + fundamentals

```
data/us_split_adjustment_factors/                 (and .../us_security_master_smoke/ — the 3-symbol smoke copy)
  security_master/security_master.parquet
    schema: date, symbol, cumulative_adjustment_factor, cash_dividend, dividend_currency,
            shares_outstanding, shares_as_of_date, shares_filed_date, sec_cik, sec_sic,
            sec_sic_description, gics_sector_code, gics_sector, gics_sub_industry, gics_source
    coverage: 5321 common-equity symbols, 1962-01-02→2026-06-12 after the S1-6 crawl  [§6.1]
              (the committed C++ test fixture .../us_security_master_smoke/ stays 3 symbols)
  factors_by_symbol/symbol=SYM/data_0.parquet     schema: symbol,date,return_factor   (canonical, split-only)
  dividends_by_symbol/symbol=SYM/data_0.parquet   schema: symbol,date,cash_dividend,currency,source
  split_events_by_symbol/, shares_outstanding_by_symbol/, sectors_by_symbol/
  manifest/  _cache/
```

The smoke copy (`data/us_security_master_smoke/`) is the on-disk fixture the S1 tests resolve (see the `find_master_parquet` / `find_data_root` probes, [`tests/data_adjust_test.cpp:120`](../../tests/data_adjust_test.cpp), [`tests/data_real_panel_e2e_test.cpp:112`](../../tests/data_real_panel_e2e_test.cpp)).

**Column semantics** (from [`docs/us_split_adjustment_factors.md`](../../../docs/us_split_adjustment_factors.md) "Limitations" + plan §0.5):
- `cumulative_adjustment_factor` = cumulative **future-split** factor. `raw_close × factor = split-adjusted close on the latest share basis`. **Split-only** by default (`--mode split`); `--mode adjclose` instead maps raw close → Yahoo adjusted close **including dividends** (the S1-3 cross-check oracle).
- `cash_dividend` = per-share cash dividend on the **ex-date**.
- `shares_outstanding` = SEC fact values **forward-filled by fact `end` date**, with `shares_filed_date` carried for provenance (the PIT leak guard — §3).
- `gics_*` is **sparse** (licensed); SEC `sec_sic`/`sec_sic_description` is the open fallback.

---

## 2. The ingestion stack layers

Two parallel ingestion rails reach the Panel. The S6 `Dataset`/`Catalog`/`adapt_*` layer is the one S1-5 actually uses; the `atx-tsdb` segment rail is the pre-S6 path (and the seam the plan originally named — see §5 for why S1-5 bypasses it).

| Layer | Symbol | `file:line` | Role |
|---|---|---|---|
| Parquet read | `atx::core::io::ParquetTable` | [`atx-core/include/atx/core/io/parquet.hpp:76`](../../../atx-core/include/atx/core/io/parquet.hpp) | lazy Arrow scan, column projection |
| Parquet read | `atx::core::io::read_parquet` | [`parquet.hpp:187`](../../../atx-core/include/atx/core/io/parquet.hpp) | file → `ParquetTable` |
| Parquet read | `ParquetTable::date32_days` / `null_mask` | [`parquet.hpp:104`](../../../atx-core/include/atx/core/io/parquet.hpp) / [`:111`](../../../atx-core/include/atx/core/io/parquet.hpp) | date32 + null support (the S1-2 atx-core lift) |
| Segment build | `atx::tsdb::load_parquet` | [`atx-tsdb/include/atx/tsdb/load_parquet.hpp:37`](../../../atx-tsdb/include/atx/tsdb/load_parquet.hpp) | long parquet → sealed `.seg` |
| Segment build | `atx::tsdb::load_parquet_scaled` | [`load_parquet.hpp:53`](../../../atx-tsdb/include/atx/tsdb/load_parquet.hpp) | i64 fixed-point × scale read |
| Segment build | `atx::tsdb::build_dated_segments` | [`load_parquet.hpp:70`](../../../atx-tsdb/include/atx/tsdb/load_parquet.hpp) | per-date `.seg` from a hive |
| Segment read | `atx::tsdb::SegmentReader` | [`segment_reader.hpp:21`](../../../atx-tsdb/include/atx/tsdb/segment_reader.hpp) | zero-copy O(1) cell access; `cutoff_index` ([`:65`](../../../atx-tsdb/include/atx/tsdb/segment_reader.hpp)) is the no-look-ahead rail |
| Segment → Panel | `alpha::attach_segment_panel` | [`alpha/segment_panel.hpp:95`](../../include/atx/engine/alpha/segment_panel.hpp) | segment → `MappedPanel` (borrowed Panel) over a `TimeWindow` ([`:31`](../../include/atx/engine/alpha/segment_panel.hpp)) |
| S6 store | `data::Dataset` | [`data/dataset.hpp:39`](../../include/atx/engine/data/dataset.hpp) | typed, PIT-versioned columnar store (Price/Feature/Signal/Reference roles, `dataset_schema.hpp`) |
| S6 registry | `data::DatasetCatalog` | [`data/catalog.hpp:46`](../../include/atx/engine/data/catalog.hpp) | named registry + as-of/lineage; `register_dataset` ([`:65`](../../include/atx/engine/data/catalog.hpp)) |
| S6 PIT align | `data::align_onto` | [`data/align.hpp:68`](../../include/atx/engine/data/align.hpp) | join a plug Dataset onto the canonical price axis → `AlignedView` ([`:53`](../../include/atx/engine/data/align.hpp)) |
| S6 → Panel | `data::price_to_panel` | [`data/adapt_panel.hpp:38`](../../include/atx/engine/data/adapt_panel.hpp) | Role::Price Dataset → augmented `alpha::Panel` (`== with_datafields`) |
| Datafield augment | `alpha::with_datafields` | [`alpha/datafields.hpp:153`](../../include/atx/engine/alpha/datafields.hpp) | append `dollar_volume`/`vwap`/`adv{w}` |

---

## 3. Corporate-action semantics

### 3.1 The adjustment-factor + dividend conventions (§0.5)

Loaded by S1-2 into a Reference-role corporate-action `Dataset` of six canonical columns, in load-bearing order ([`data/corporate_actions.hpp:49`–52](../../include/atx/engine/data/corporate_actions.hpp), `kCol*` constants [`:78`–84](../../include/atx/engine/data/corporate_actions.hpp)): `cum_adj_factor`, `cash_dividend`, `shares_outstanding`, `shares_filed_date`, `gics_sector_code`, `sic_code`. Missing-data policy is **never coerce to 0** ([`corporate_actions.hpp:24`–29](../../include/atx/engine/data/corporate_actions.hpp)): absent dividend → `0.0`; absent/not-yet-filed shares → `NaN`; absent factor → `NaN` (a missing split factor is *unknown*, not `1.0`); absent sector → the `kNoSector` (`-1.0`) sentinel ([`corporate_actions.hpp:73`](../../include/atx/engine/data/corporate_actions.hpp)). A non-USD `dividend_currency` fails the load closed ([`src/data/corporate_actions.cpp:86`](../../src/data/corporate_actions.cpp)).

### 3.2 The total-return model (§0.7 #1)

> **§0.7 #1 — Adjustment = total-return.** The canonical price field consumed downstream is a total-return index (split + reinvested-dividend adjusted). Raw close and split-adjusted close are retained as additional fields for signals that explicitly want them.

S1-3 `adjust_total_return` ([`data/adjust.hpp:121`](../../include/atx/engine/data/adjust.hpp)) folds split + reinvested dividends into one symbol's total-return index ([`adjust.hpp:13`–36](../../include/atx/engine/data/adjust.hpp)):

1. **Split-adjusted close** `S_t = raw_close_t × cum_adj_factor_t` — continuous across a split (the factor step cancels the ex-date price drop). Validated at the AAPL 4:1 (2020-08-31) and 7:1 (2014-06-09) ex-dates ([`tests/data_adjust_test.cpp:206`](../../tests/data_adjust_test.cpp) `SplitAdjustedNoDiscontinuityAtKnownAaplSplits`).
2. **Daily total return** `r_t = (S_t + D_t·cum_adj_factor_t) / S_{t-1} − 1`, `r_0 = 0` — additive dividend reinvestment in the numerator (no `1 − D/C` negative-factor pathology).
3. **Total-return index** `TRI_0 = S_0`, `TRI_t = TRI_{t-1}·(1 + r_t)` — a geometric chain; the absolute level is arbitrary (anchored to the first valid `S`), only the returns are economically meaningful.

The result `AdjustedSeries` ([`adjust.hpp:88`](../../include/atx/engine/data/adjust.hpp)) carries `split_adj_close`, `total_return_index` (the canonical `close`), and `total_return`. NaN policy: a NaN raw close **or** a NaN `cum_adj_factor` is a *gap* (NaN S/r/TRI, never zero-filled), and the series re-anchors at the next valid close ([`adjust.hpp:50`–62](../../include/atx/engine/data/adjust.hpp); [`tests/data_adjust_test.cpp:381`](../../tests/data_adjust_test.cpp) `NanRawCloseDoesNotZeroFill`).

### 3.3 Back-adjustment is return-invariant ⇒ non-leaking (§0.7 #2)

> **§0.7 #2 — Back-adjustment is return-invariant ⇒ non-leaking.** Full-history back-adjustment rescales price *levels* when a future split/dividend arrives, but **returns are invariant to a constant multiplicative rescale**. Alphas consume returns/ranks, so back-adjustment does not leak signal. **The PIT leak guard is on the as-of join of fundamentals** (`shares_outstanding`, sector) using `shares_filed_date`/knowledge-date — **not** on price adjustment.

**Why it holds, mechanically.** Scaling the entire `raw_close` history (and the price-denominated dividend) by any positive constant `k` leaves `total_return` byte-identical: `S_t` scales to `k·S_t` and `D_t_adj` is on the same basis, so `r_t = (k·S_t + k·D_t_adj)/(k·S_{t-1}) − 1` is independent of `k` ([`data/adjust.hpp:38`–48](../../include/atx/engine/data/adjust.hpp) "THE RETURN-INVARIANCE NON-LEAK CONTRACT"). Back-adjustment therefore rescales *levels* but not *returns*; alphas consume returns/ranks, so full-history back-adjustment does not leak the future. This is asserted bit-for-bit by [`tests/data_adjust_test.cpp:346`](../../tests/data_adjust_test.cpp) `ReturnInvariantUnderConstantPriceRescale` (rescales close **and** the price-denominated dividend by an arbitrary `k`, then `bit_cast<u64>`-compares every `total_return`).

**Where the real PIT leak guard lives — on the fundamental as-of join, not on price.** `shares_outstanding` and sector are *forecasts-as-of a filing*: a row dated `d` may carry a value only made public on a **later** date. S1-2 forward-fills each fundamental using **only** filings whose knowledge-date `shares_filed_date ≤ d` — a value is invisible on every date before its filing date ([`corporate_actions.hpp:13`–19](../../include/atx/engine/data/corporate_actions.hpp); the as-of resolution is `pit_fill_symbol`, [`src/data/corporate_actions.cpp:259`](../../src/data/corporate_actions.cpp), with the greatest-`filed_date ≤ d` upper-bound step at [`corporate_actions.cpp:283`–291](../../src/data/corporate_actions.cpp)). Corporate-action facts (`cum_adj_factor`, `cash_dividend`) are mechanical ex-date facts — joined on their own event date with no knowledge-date guard. This is the asymmetry §0.7 #2 names: **price adjustment carries no leak (return-invariant); the fundamental join is where the leak guard is enforced.**

---

## 4. Universe construction

S1-4 `build_universe` ([`data/universe.hpp:127`](../../include/atx/engine/data/universe.hpp)) derives four date×instrument fields ([`UniverseFields`, universe.hpp:107](../../include/atx/engine/data/universe.hpp)) from an axis-matched price `Panel` + corp-action `Dataset`, tuned by `UniverseConfig` ([`universe.hpp:92`](../../include/atx/engine/data/universe.hpp)):

- **`market_cap` = shares_outstanding (PIT as-of) × RAW close** — split-invariant by construction (a 2:1 split halves price and doubles shares; the product is unchanged), so it uses the **raw**, never the adjusted, close ([`universe.hpp:18`–25](../../include/atx/engine/data/universe.hpp); `market_cap_field`, [`src/data/universe.cpp:97`](../../src/data/universe.cpp)). NaN if either input is NaN ([`tests/data_universe_test.cpp:140`](../../tests/data_universe_test.cpp) `MarketCapIsSplitInvariant`).
- **`adv_usd`** — causal trailing mean of `dollar_volume` over `adv_window` bars, consumed from the Panel's pre-computed `adv{w}` datafield when present, else recomputed with the same causal rolling mean ([`universe.hpp:27`–35](../../include/atx/engine/data/universe.hpp); `causal_rolling_mean`, [`src/data/universe.cpp:65`](../../src/data/universe.cpp)). No look-ahead: the window reads only dates ≤ t, so truncating future dates leaves a past date's ADV byte-identical ([`tests/data_universe_test.cpp:164`](../../tests/data_universe_test.cpp) `AdvIsCausalNoLookAhead`).
- **`sector_code`** — GICS if present, else SIC fallback, else the `kNoSectorCode` (`-1`) sentinel — never `0` ([`universe.hpp:37`–41](../../include/atx/engine/data/universe.hpp); `resolve_sector`, [`src/data/universe.cpp:136`](../../src/data/universe.cpp)).
- **`in_universe`** — the PIT membership mask: `present(t,i) ∧ market_cap ≥ min_mktcap_usd ∧ adv_usd ≥ min_adv_usd ∧ (optional top-N-by-ADV)`. A NaN cap/ADV fails its floor (NaN compares false), so a NaN price/shares cell is excluded; the top-N tie-break is ascending canonical instrument id (deterministic) ([`membership_mask`, src/data/universe.cpp:187](../../src/data/universe.cpp); `apply_top_n`, [`universe.cpp:160`](../../src/data/universe.cpp)).

The unit is a **pure transform over matching axes** — `corp_actions` must already be aligned onto the price-defined canonical axis; a shape mismatch fails closed with `Err(InvalidArgument)` ([`validate_axes`, src/data/universe.cpp:213](../../src/data/universe.cpp)). S1-5 owns the `align_onto` projection (§5).

### The survivorship caveat (documented, not fixed)

The universe screen operates **only over the symbols present in the listed-only security master** (Nasdaq Trader active symbols; [`docs/us_split_adjustment_factors.md`](../../../docs/us_split_adjustment_factors.md) "Limitations"). A delisted name simply never appears in the corp-action `Dataset`, so it is silently absent from every universe — a **survivorship bias** that flatters any backtest run over this universe. S1-4 **documents** this bias; it does **not** fix it (recovering delisted equities needs a delisting-event source, backlogged to P4). The in-code statement is the `universe.hpp` header doc-comment ([`data/universe.hpp:43`–54](../../include/atx/engine/data/universe.hpp) "SURVIVORSHIP CAVEAT") and the S1-4 ledger row ([`sprint-1-progress.md`](sprint-1-progress.md), the `S1-4` row); this section is the canonical statement that the `DataUniverse.SurvivorshipCaveatDocumentedOrDeferred` test ([`tests/data_universe_test.cpp:318`](../../tests/data_universe_test.cpp)) greps for. The bias is reported as a **first-class result** in the S2 scorecard (ROADMAP §"Strategic positioning"), never hidden.

---

## 5. The end-to-end real-data path (how to get a real-data Panel)

S1-5 `build_real_panel(RealDataConfig)` ([`data/real_panel.hpp:140`](../../include/atx/engine/data/real_panel.hpp)) is the canonical recipe — it *orchestrates* the shipped seams into one deterministic `alpha::Panel` plus a golden digest; it re-implements none of them ([`real_panel.hpp:5`–13](../../include/atx/engine/data/real_panel.hpp)). `RealDataConfig` ([`real_panel.hpp:106`](../../include/atx/engine/data/real_panel.hpp)) carries the databento hive root, the security-master path, the `TimeWindow`, and the `UniverseConfig`. The result `RealPanel` ([`real_panel.hpp:126`](../../include/atx/engine/data/real_panel.hpp)) carries the `panel`, the `digest`, and the catalog `lineage`.

**Assembly (fixed order, `src/data/real_panel.cpp:349` `build_real_panel`):**

1. **Corp Dataset + canonical InstKey map** — `load_security_master` ([`real_panel.cpp:355`](../../src/data/real_panel.cpp)) + `canonical_instruments` ([`real_panel.cpp:88`](../../src/data/real_panel.cpp)): the master's **first-seen** intern order defines the symbol → `InstKey` map every other axis reuses (`align_onto` joins by matching `InstKey`, not by position).
2. **Price Dataset (the canonical axis)** — `build_price_dataset` ([`real_panel.cpp:201`](../../src/data/real_panel.cpp)) reads the databento parquet(s) over the window, restricted to the corp symbol set, into a `Role::Price` Dataset on the same InstKeys. **Price defines the canonical axis (§0.7 #3): a (symbol,date) absent from price is absent from the Panel.**
3. **Augmented Panel** — `price_to_panel` ([`real_panel.cpp:362`](../../src/data/real_panel.cpp)) → a Panel carrying `dollar_volume`/`vwap`/`adv{adv_window}`.
4. **Total-return adjust** — `align_onto(price, corp)` ([`real_panel.cpp:365`](../../src/data/real_panel.cpp)) re-expresses the corp columns on the price axis, then per symbol `adjust_total_return(raw_close, cum_adj_factor, cash_dividend)` (S1-3) → `total_return_index` (the canonical `close`) with `raw_close` retained (`adjust_per_symbol`, [`real_panel.cpp:264`](../../src/data/real_panel.cpp)).
5. **Universe screen** — `build_universe` (S1-4) over the augmented Panel + an axis-matched corp Dataset rebuilt from the `AlignedView` (`corp_on_price_axis`, [`real_panel.cpp:300`](../../src/data/real_panel.cpp)); `in_universe` becomes the Panel mask.
6. **Catalog + digest** — register `price`/`corp_actions`/`universe` in a `DatasetCatalog`, record lineage, assemble the final Panel in canonical field order (`kFieldClose`…`kFieldSector`, [`real_panel.hpp:83`–90](../../include/atx/engine/data/real_panel.hpp)), and digest it (`digest_panel`, [`real_panel.cpp:331`](../../src/data/real_panel.cpp)).

**Determinism pin.** `digest` is `signal_set_digest` over the Panel's fields in canonical order ([`real_panel.hpp:35`–42](../../include/atx/engine/data/real_panel.hpp)). The golden pin is **`0x2a22a873483d9157`** over the 3-symbol smoke window `[2024-07-01, 2024-08-01)` (22 trading dates), with a second build asserted byte-identical ([`tests/data_real_panel_e2e_test.cpp:169`](../../tests/data_real_panel_e2e_test.cpp) `kGoldenDigest`; test `BuildsSmokeRealPanelDeterministicDigest`, [`:183`](../../tests/data_real_panel_e2e_test.cpp)).

> **As-built fact — `build_real_panel` reads the databento parquet directly, NOT via `build_dated_segments`/`attach_segment_panel`.** The plan's original step-1/2 named the `atx-tsdb` segment seam. As-built, those two seams cannot consume the real databento hive: `build_dated_segments` hard-codes `data.parquet` per partition (the real hive holds `part-00000.parquet`), and `load_parquet_scaled` reads each field as **i64 fixed-point × scale** while the real OHLCV columns are **f64 dollars** (the i64 read fails). So S1-5 reads the actual parquet directly through `atx::core::io::read_parquet` and lands it through the *same* `adapt_panel`/`align`/`universe`/`catalog` layer the plan specifies; price still defines the canonical axis, and `seg_cache_dir` is retained in the API for stability but is **not written** (there is no `.seg` round-trip). This divergence is recorded verbatim in the file header ([`src/data/real_panel.cpp:7`–19](../../src/data/real_panel.cpp)) and the S1-5 ledger row.

---

## 6. Coverage / rebuild

The security master is produced by the python builder `python/scripts/build_us_split_adjustments.py` (in the repo root tree; not part of this engine branch). The default output root is `data/us_split_adjustment_factors/` and the joined master is `security_master/security_master.parquet`. Full reference: [`docs/us_split_adjustment_factors.md`](../../../docs/us_split_adjustment_factors.md).

**Mode (`--mode`, choices `split`|`adjclose`, default `split`):**
- `--mode split` (default) — `cumulative_adjustment_factor` is the cumulative **future-split** factor; `raw_close × factor = split-adjusted close`. This is what S1-2/S1-3 consume.
- `--mode adjclose` — instead yields the factor mapping raw close → Yahoo adjusted close **including dividends**; this is the S1-3 cross-check **oracle** (the documented hand-fixture fallback is used in lieu of the web crawl in the tests).

**Common rebuild invocations** (mirroring the builder doc):
```powershell
# Full refresh of the common-only listed universe:
python scripts/build_us_split_adjustments.py --common-only --overwrite

# Incremental (self-healing) refresh of named symbols:
python scripts/build_us_split_adjustments.py --symbols AAPL,MSFT --datasets yahoo sec security-master --incremental

# Rebuild only the final joined master from existing canonical partitions:
python scripts/build_us_split_adjustments.py --datasets security-master --overwrite
```
- `--datasets` (choices `all`|`yahoo`|`sec`|`security-master`): `yahoo` writes factors/dividends/splits; `sec` writes shares/sectors; `security-master` writes the final joined file.
- `--symbols` (comma/space separated) or `--symbol-file`; `--common-only` keeps common/ordinary/ADS issues; `--incremental` preserves existing partitions and rebuilds only the named symbols (`--force-symbols` overrides the planner's "already complete" skip).

**Polite-crawl flags** (open Yahoo/SEC sources — keep within the published ceilings; cite the real argparse names, [`build_us_split_adjustments.py`](../../../python/scripts/build_us_split_adjustments.py) argparse):
- `--yahoo-min-interval` (default `0.20`) — global minimum seconds between uncached Yahoo requests across all workers.
- `--sec-min-interval` (default `0.35`) — global minimum seconds between uncached SEC requests.
- `--request-delay` (default `0.05`) / `--sec-request-delay` (default `0.12`) — small randomized per-request delay (Yahoo / SEC).
- `--backoff-max-seconds` (default `180.0`) — max sleep between retries after a transient HTTP failure / rate-limit.
- `--sec-user-agent` (default `"atx-security-master/0.1 contact@example.com"`) — **set this to a real app/contact string for production runs** (SEC EDGAR requires it).

**Known coverage gaps** (the §4 survivorship caveat + the licensed-GICS sparsity): after the S1-6 crawl the on-disk master is **5321 common-equity symbols** (§6.1); `gics_*` is licensed/sparse (non-null on ~18% of rows) with SEC SIC as the open fallback; the universe is listed-only (the survivorship bias above). The committed C++ **test fixture** stays the 3-symbol smoke master at `data/us_security_master_smoke/` — the E2E digest pin is against the smoke, not the expanded master.

### 6.1 S1-6 status — coverage expansion **run** (2026-06-15)

S1-6 re-runs the builder over the databento universe so the S2 benchmark has corporate-action coverage on a **liquid subset** rather than the smoke 3. It is a live, identity-bearing crawl of **open Yahoo + SEC EDGAR sources**; per the plan it is independent, non-blocking, and partial-tolerant. It was **deferred at S1-close** (commit `e3f01f8`) and then **run post-merge on 2026-06-15** on operator request. The result is a data-build only: the output lives in gitignored `data/`, so there is no engine commit and no test change.

**Selection rule (reproducible).** `python/scripts/derive_liquid_universe.py` ranks the databento `equs_ohlcv_1d` universe by **trailing-21d average dollar-volume** (close×volume over the most recent 21 date partitions) and intersects it with the Nasdaq-Trader **common-equity** set — reusing the builder's own `looks_common_equity` (drops ETFs / test issues / non-common issues) — ties broken by ascending canonical symbol, keeping the **top-500**. Cutoff ≈ $277M/day; top-10 = MU, NVDA, TSLA, SNDK, AMD, MSFT, INTC, AAPL, GOOGL, AVGO. This is the same causal `dollar_volume`/`adv{21}` liquidity measure §4 uses.

**Command run** (polite defaults — `--yahoo-min-interval 0.20`, `--sec-min-interval 0.35`, `--backoff-max-seconds 180`):
```powershell
python python/scripts/derive_liquid_universe.py --top-n 500 --out data/databento_liquid_top500.txt
python python/scripts/build_us_split_adjustments.py --symbol-file data/databento_liquid_top500.txt --datasets all --mode split --incremental --sec-user-agent "atx-research <nathan.tormaschy@gmail.com>"
```

**Outcome.** Yahoo built 500/500 (10 batches, ~40s); the SEC planner reported 500 already complete (prior-crawl partitions on disk), so no fresh EDGAR requests were issued; the `security-master` join then rebuilt the master from **all** on-disk by-symbol partitions. The expanded `data/us_split_adjustment_factors/security_master/security_master.parquet` carries **5321 symbols** (the full common-equity universe present on disk; the top-500 liquid subset all present), **23,197,482 rows**, dates **1962-01-02 → 2026-06-12**, **schema parity with the smoke master = true**, `gics_sector_code` non-null on 4.28M/23.2M rows, `shares_outstanding` for 4303/5321 symbols, `dividend_currency ∈ {None, USD}`. Spot-check: AAPL 11,467 rows → 2026-06-12, last `cum_adj_factor`=1.0, last `shares_outstanding`=14,687,356,000 (matches the S1-2-verified 14.687B filing). `--incremental` was non-destructive (prior partitions preserved, only the 500 refreshed).

**To refresh / re-run later:** re-issue the two commands above (the ranker is deterministic over the current hive; `--incremental` self-heals). Before pinning S2 results to the wider universe, re-verify row/symbol/date counts + schema parity and spot-check 2–3 names against Yahoo adjusted closes.

---

## 7. The ORATS single-file source (p3 S3) — `tbltickerhistory`

S3 adds a **second, self-contained** real-data source: the ORATS SMV `tbltickerhistory` export, one ~3.3 GB zip whose single TSV entry (~11 GB inflated, 71 columns, date-major) carries **price + corporate-action factors + universe fields in one file** — no databento⋈master join. Anchors below are at the S3 close HEAD; paths are repo-root-relative.

### 7.1 The source file (§ recon)

`~/Downloads/tbltickerhistory3_10y.zip` → single entry `tbltickerhistory3_10y.txt` (11,084,562,320 bytes, TSV, 71 cols, **date-major** = non-decreasing `tradingDate`). ORATS columns of interest: `securityID` (stable PIT id), `ticker_tk`/`todayTicker` (symbols), OHLC, `closePr`, `closeUnadjPr`, `volume`, `shares`, `GICS`, `returnFactor`, `totalReturn`, **`cumulReturnFactor`** (total-return back-adjustment factor, col 65), `atmCenI_*` (IV term structure), `nEarnCnt_*` (earnings counts). The 11 GB body is **streamed**, never materialized.

### 7.2 The loader chain (atx-core primitive → atx-engine loader → Panel)

| Stage | Symbol | Where |
|------|--------|-------|
| Streaming zip inflate | `io::ZipEntryReader` (pImpl; miniz confined to `.cpp`, PRIVATE to atx-core) | [`atx-core/include/atx/core/io/zip_reader.hpp:12`](../../../atx-core/include/atx/core/io/zip_reader.hpp#L12), `src/io/zip_reader.cpp` |
| Zip TSV → per-date `.seg` partition + side-cars | `data::load_orats_history(OratsLoadConfig)→Result<OratsLoadStats>` | [`atx-engine/include/atx/engine/data/orats_history.hpp:61`](../../include/atx/engine/data/orats_history.hpp#L61), `src/data/orats_history.cpp` |
| Corporate-action TRI close | `data::orats_total_return_close(close, cumReturnFactor)→vector<f64>` | [`atx-engine/include/atx/engine/data/history_panel.hpp:70`](../../include/atx/engine/data/history_panel.hpp#L70), `src/data/history_panel.cpp:39` |
| Union per-date segments → one owned Panel (the interface expansion) | `alpha::attach_multi_segment_panel(seg_dir, window, fields, universe)→Result<Panel>` | [`atx-engine/include/atx/engine/alpha/segment_panel.hpp:112`](../../include/atx/engine/alpha/segment_panel.hpp#L112) |
| Orchestrator → digest-pinned `alpha::Panel` | `data::build_history_panel(HistoryDataConfig)→Result<HistoryPanel>` | [`atx-engine/include/atx/engine/data/history_panel.hpp:83`](../../include/atx/engine/data/history_panel.hpp#L83), `src/data/history_panel.cpp:74` |
| Determinism oracle (shared with `build_real_panel`) | `data::digest_panel(const alpha::Panel&)→u64` | [`atx-engine/include/atx/engine/data/panel_digest.hpp:27`](../../include/atx/engine/data/panel_digest.hpp#L27) |

`load_orats_history` writes `<out_dir>/YYYY-MM-DD.seg` per trading date (symbol name = **`securityID`**, stringified — survivorship-correct across renames/delistings), plus `<out_dir>/_symbology.parquet` (securityID, ticker_tk, todayTicker — **sorted by securityID** for byte-reproducibility) and `<out_dir>/_manifest.json`. Input **must** be date-major; a date regression fails closed with `Err(InvalidArgument)` *before* the date-floor filter, and the loader holds the counting invariant `rows_read == rows_filtered + rows_malformed + rows_kept`.

### 7.3 The 16-field projection + the 15-char segment-name constraint

Only **16** of the 71 columns are projected into the segment, in a fixed canonical (digest-stable) order — `kOratsFields` ([`orats_history.hpp:21`](../../include/atx/engine/data/orats_history.hpp#L21)):

`open, high, low, close, closePr, closeUnadjPr, volume, shares, returnFactor, totalReturn, cumReturnFactor, gics, earnFlag, atmCenI_21d, atmCenI_126d, nEarnCnt_5d`

Forward-looking columns (`hEMove, iEMove, shD1, lnD1, wkD1, qtrD1, *EMove`) are **excluded** (leak risk). Per the user constraint, only `atmCenI_21d`/`atmCenI_126d` (not the full surface) and `nEarnCnt_5d` (not the full count set) are taken.

**The cumReturnFactor name.** The atx-tsdb segment format NUL-pads field names into a 16-byte buffer (`kFieldNameLen=16` ⇒ ≤15 usable chars), so the TSV column `cumulReturnFactor` (17) would **silently truncate** on write and never resolve on read-back. The **segment** field is therefore named `cumReturnFactor` (15), guarded at compile time by a `consteval` over `kOratsFields` ([`orats_history.hpp:43`](../../include/atx/engine/data/orats_history.hpp#L43) — any future over-long name fails the build). `detail::resolve_header` maps the 15-char segment name back to the real TSV header `cumulReturnFactor` (the same special-case mechanism as `gics→GICS`), keyed off `kOratsFields[10]` so the remap tracks a rename automatically.

### 7.4 Corporate-action semantics (the `cumulReturnFactor` TRI)

Canonical adjusted close = **`close × cumulReturnFactor`** — `close` is col 9 (the as-traded **contemporaneous** close); `cumulReturnFactor` is the total-return back-adjustment factor on the **same row**. `closeUnadjPr` is the **lagged prior-day close**, *not* the adjustment input — it is retained as a raw field but never multiplied. Implemented by reusing the S1-3 `adjust_total_return(close, cumReturnFactor, zeros)` whose `total_return_index` equals `close × cumReturnFactor` exactly ([`history_panel.cpp:39`](../../src/data/history_panel.cpp#L39)); the orchestrator applies it once per instrument over the date-major stride ([`history_panel.cpp:168`](../../src/data/history_panel.cpp#L168)). Raw `close` is retained as `raw_close`. **Verified:** AAPL 2012-03-26 `606.98 × 0.0299354 = 18.17` ≈ Yahoo present-basis adjusted close (`data_history_tri_test.cpp`). This is the **same return-invariance argument** as §3.3 — alphas consume returns/ranks, and a constant multiplicative rescale leaves returns invariant, so full-history back-adjustment leaks no signal.

### 7.5 The assembled Panel (canonical field order) + determinism

`build_history_panel` mirrors `build_real_panel`'s assembly but sources from `attach_multi_segment_panel` instead of the databento hive. The Panel's fields, in the **digest-load-bearing** order ([`history_panel.hpp:32`](../../include/atx/engine/data/history_panel.hpp#L32)):

| Field | Meaning |
|------|---------|
| `close` | **TRI** = `close × cumulReturnFactor` (the canonical adjusted price) |
| `raw_close` | raw as-traded ORATS close |
| `volume`, `high`, `low`, `open` | raw ORATS OHLCV |
| `market_cap` | `shares` × raw close (via `build_universe`) |
| `sector` | GICS sector code (NaN→`-1` sentinel) |

`digest` is `digest_panel` (`signal_set_digest` over the fields in canonical order) — the **shared** oracle extracted from `real_panel.cpp` (S3-5 Step 0), so the S1-5 golden pin `0x2a22a873483d9157` is unchanged. The history path's own determinism is self-checked by `DataHistoryPanel.DeterministicDigestAndCanonicalFields`.

### 7.6 The end-to-end path + operator data-build

The unchanged p2 `factory::RobustResearchDriver` consumes `build_history_panel`'s Panel directly — proven by `OratsE2ESmoke.SyntheticPartitionRunsUnchangedRobustPipeline` (always-run: a synthetic 60-date × 30-instrument partition → `build_history_panel` → `driver.run`, asserting a non-zero research digest), plus two **guarded** modes that skip in CI: `RealPartitionRunsUnchangedRobustPipeline` (probes `data/orats_history_1d`) and `OperatorOratsZip` (the committed operator caller, keyed on env `ATX_ORATS_ZIP`).

**To run the real 11 GB build** (gitignored output; ~minutes single-threaded), set the env var and run the operator test:
```powershell
$env:ATX_ORATS_ZIP = "$HOME\Downloads\tbltickerhistory3_10y.zip"
$env:ATX_DATA_DIR  = "C:\atx-data"   # parent of orats_history_1d; keep OFF OneDrive
& scripts/dev-build.ps1 -Build -Test -TestRegex "OratsE2ESmoke.OperatorOratsZip"
```
`OperatorOratsZip` constructs `OratsLoadConfig{ zip_path=$ATX_ORATS_ZIP, out_dir=$ATX_DATA_DIR/orats_history_1d, min_date_nanos=date_to_nanos("2020-01-01"), created_at_nanos=0 }`, calls `load_orats_history`, then `build_history_panel` + `driver.run`, and logs `OratsLoadStats` (rows_read/kept/filtered/malformed, dates_written, distinct securities) via `GTEST_LOG_`. The **date floor is 2020-01-01** (user constraint). Record the stats in the ledger; do not commit `data/`. As of S3 close this build is **deferred** (documented manual step) — the code path is proven by the synthetic always-run test.
