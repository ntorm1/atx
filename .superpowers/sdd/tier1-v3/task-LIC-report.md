# task-LIC report: S8.1 licensed adapters, S8.2 options (tier1-v3)

Lane LIC. Branch `feat/tier1-v3-warehouse`. Commits: `36d0f2d5` (S8.1), `f79ef330` (S8.2), plus the report commit.
Doc: `atx-db/docs/LICENSED_ADAPTERS.md`.

## 1. What was built

**`atx_db/licensed/` (S8.1).** One shared contract plus one module per licensed domain.
- `contract.py`: keys `security_id` / `cik` with `link_tier` and `cik_link_tier`; clocks `vendor_snapshot_at`,
  `delivered_at`, `available_at`, `clock_basis`, `history_mode`, `vintage_risk`; lineage.
  - `history_mode` (from `receipts.jsonl`): `pit_archive` takes the product rule clock; `daily` and `backfill` take
    `greatest(rule, delivery)`. A file without a receipt is a `backfill` delivered at build time, so a vendor as-of
    re-cut never reaches the past.
  - `load(raw_dir) -> Stage` (Parquet with row groups ≤ 32k, then a manifest with the code, output, raw-input and
    identity-input SHA-256s).
  - `validate`: schema, unique keys, clocks present, snapshot ≤ available, no future clocks, clocks monotone per
    vintage series, link-tier consistency, clock domains, plus adapter checks. Rejects are counted per reason.
- `identity.py`: rule `licensed-id-v1`. Tiers in order: `vendor_native`, `cusip_dated`, `cusip_undated` (within 400
  days), `ticker_dated`, then `ambiguous` or `unmapped`.
  - Histories come from the lake: FTD and 13F CUSIP maps, ticker runs from prices, and `identity/link_table.parquet`.
    It switches to lane ID's `security_master/{cusip,ticker}_history.parquet` when those exist.
  - The CIK link bridges gaps of up to 10 days.
  - A suffixed vendor ticker (`NE.WT`) never matches a line without a suffix (`NEWT`).
- `mock.py`: a deterministic universe covering a ticker change, a CUSIP change, a delisting, ticker reuse, and a
  2-class issuer. CUSIP and ISIN check digits are valid.
- Adapters. Each has a vendor-layout mock and a `substitute()`:

  | adapter | module | substitute |
  |---|---|---|
  | estimates: consensus, detail, actuals, recommendations, price targets (I/B/E/S WRDS layout) | `estimates.py` | EVT `events/guidance.parquet` |
  | securities lending | `lending.py` | `borrow_proxy/` |
  | GICS, with a structure guard | `gics.py` | MKT `classification/issuer_industry.parquet` |
  | index constituents and weights | `indexes.py` | MKT `indexes/constituents.parquet` |
  | transcripts | `transcripts.py` | none (plan §1.2) |

  EVT and MKT confirmed their planned column names and the mappings follow them.
- CLI: `python -m atx_db.licensed {load,mock,substitutes}`.

**S8.2 options.**
- `licensed/options.py`: mock-backed adapter for SpiderRock `OptionEODFeaturesHist` + `SurfaceFixedGridHist`. Output
  is per (security_id, session): 25-delta put/call IV at 30 d, skew, term slope, call/put volume and OI. Identity is
  the native securityID.
- `alpha_panel/options.py`: stage `options/` built from the free ATM term structure (`iv_atm_{21,63,126,252}d`,
  `term_slope_63_21`, `term_slope_252_21`). The licensed columns stay NULL until `--licensed` is supplied.
  - Clock `options-clock-v1`: session date 22:00 America/Chicago, which is the vendor's documented T+0 US delivery.
  - Output: 10,656,780 rows, 9,036 lines, 2018-01-02..2026-09-18, 236 MB, 9 year files. No duplicate key, no NULL
    clock. Run time 71 s, peak 0.35 GiB under the guard (rebuilt 2026-09-30 00:20Z against the final panel v2).

**Tests.** 52 in 8 files: `test_licensed_{contract,estimates,lending,gics,indexes,transcripts,options}.py` and
`test_alpha_panel_options.py`. All offline; 2-4 s per file.

## 2. Done criteria

| criterion | result | measured | command |
|---|---|---|---|
| S8.1 contract tests pass on mocks for every adapter: schema, PIT, identifier mapping, substitute wiring | PASS | 52/52 tests pass; 0 fatal validation failures on every mock; ruff clean | `pytest tests/test_licensed_*.py tests/test_alpha_panel_options.py -n 0` |
| S8.1 each adapter has a Parquet schema with keys + `available_at` + vendor snapshot, a loader, a mock, validation, and a substitute where one exists | PASS | 6 adapters, 11 tables; `test_registry_contract` checks every table for the ID, clock and lineage fields, keys and series | same |
| S8.1 validation catches each violation | PASS | each check fails on an injected fault: duplicate key, future clock, snapshot > available, older vintage visible later, bad link tier, wrong schema; strict load raises on a future STATPERS | `test_validation_detects_each_violation`, `test_strict_load_raises_on_future_vendor_dates` |
| S8.1 identifier mapping via CUSIP/ticker history with `link_tier` | PASS (mocks); real lake smoke below | mock consensus: 99.9% mapped; every tier exercised (dated, undated, ticker, ambiguous, unmapped, native). Real-lake smoke: 10,438 SEC tickers resolved on 2026-09-18, 6,521 mapped `ticker_dated` (the 3,917 unmapped are OTC, fund and unlisted tickers absent from the vendor file), CIK agrees with SEC on 5,578 / 5,580 = 99.96% (99.91% before the suffix rule) | `scratchpad/identity_smoke.py` under the guard (0.21 GiB, 8 s) |
| S8.1 `LICENSED_ADAPTERS.md` states each PIT rule and the D3 purchase decision points | PASS | §1 contract; §2.1-2.6 per-adapter PIT rules; §3 six ordered decision points with acceptance gates | doc |
| S8.2 find what option data atx-vol keeps and where `iv_atm_*` comes from | PASS | atx-vol was removed 2026-09-19 (`e4bdcf54`) and kept no per-security surface history. `C:/atx-data` (its OPRA hive) is empty; only 3 sessions remain in `C:/atx-scratch/opra-hive-tail`. `iv_atm_*` = TickerHistory3 `atmCenI_*` (earnings-censored ATM IV), which has no delta IV, volume or OI | git history, disk listing, vendor dictionary |
| S8.2 ≥ 95% of optionable member lines covered, **or** the documented license need | license need documented (skew/volume/OI); the free ATM level and term slope pass | term slope covers 99.6-99.9% of optionable member cells and 99.9-100% of optionable member lines, every year 2018-2026. Optionable cells are 95.7-98.4% of member_equity cells. Skew, volume and OI coverage is 0 | `options/manifest.json` `coverage` |

## 3. Sources

No data was landed and no purchase was made (D3).
- Web documentation only: the SpiderRock historical data dictionaries for TickerHistory3, SurfaceFixedTermHist,
  SurfaceFixedGridHist, SurfaceCurveHist, OptionCloseMarkHist and OptionEODFeaturesHist, plus the index page
  (7 page reads, sequential).
- Facts used:
  - US EOD delivery is 22:00 CT T+0.
  - OptionEODFeaturesHist starts 2014-01-02; it carries call/put volume and OI, `atmI_*` and `delta20Skew*`.
  - SurfaceFixedGridHist starts 2010-01-04; it carries nine call-delta vols at 12 fixed terms.
  - OptionCloseMarkHist starts 2014-01-02 and carries OI (one day delayed).
- Existing local asset noted for S8.3 cross-checks: `Downloads/ORATS_SMV_Strikes_20240103.zip` (one day of strikes).
- Disk: +236 MB (`options/`). Mocks were written only to the scratchpad and pytest temp directories.

## 4. Deviations and open issues

1. **Panel IV clock (controller, `export_impl.py`).**
   - The panel labels `iv_atm_*` `vendor-eod-same-date`, i.e. usable at the next session.
   - The vendor documents delivery of its EOD history at 22:00 CT (03:00/04:00 UTC the next day), which is after the
     next session's 22:00 UTC cutoff.
   - Under the delivery clock these values are usable one session later than the panel assumes. The `options/`
     stage uses the delivery clock.
2. **Panel dependency.** Coverage is computed against panel v2 (its manifest SHA is in `options/manifest.json`). A
   later panel rebuild changes only the coverage numbers; rerun `python -m atx_db.alpha_panel.options` under the
   guard (~70 s) after one.
3. **CLAUDE.md is stale.** The brief and `C:/atx/CLAUDE.md` point to `atx-vol/CLAUDE.md`, which no longer exists
   (removed in `e4bdcf54`).
4. **Link-table CIK mismatches (lane ID).** The real-lake smoke found two CIKs that differ from SEC's current
   mapping, both at `name` tier: FSSL 1501729 vs SEC 2065812, and CBAT 1117171 vs SEC 2086841. Both look like issuers
   that re-registered under a new CIK.
   - The other three mismatches (NE.WT, BC.PC, T.PC) came from suffix collisions in the canonical ticker form. They
     are fixed in `f79ef330`, with tests.
5. **Unguarded reads.** A few short read-only inspection queries on lake stages (schema/row counts, < 5 s, ≤ 300 MB
   DuckDB) ran without the guard while exploring and verifying. All builds and smokes ran guarded.
6. **Options stage size.** `options/` repeats prices' ATM IV columns (236 MB) so that the licensed columns have a
   home with explicit clocks. If disk is tighter than that, the free part can be read from `prices/` and the stage
   kept licensed-only.
7. **Load size.** `load()` materializes each normalized table in memory. A multi-year I/B/E/S detail delivery should
   be loaded per year (see doc §4), or the loader extended to stream by file, before S8.3.
8. **Vendor field names to confirm at purchase.** The mocks follow public dictionaries; the aliases absorb spelling
   differences. The fields to confirm:
   - SpiderRock grid delta convention and term column;
   - S&P Securities Finance field names;
   - the GICS structure-break seed.
