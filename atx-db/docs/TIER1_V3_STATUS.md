# Tier-1 v3 warehouse: status at the owner stop (2026-09-30, ~01:45 UTC)

Plan: `docs/superpowers/plans/2026-09-28-tier1-v3-parity-warehouse.md`. Branch `feat/tier1-v3-warehouse` (45 commits
over `main` 7fbfc379, not merged). Ledger: `.superpowers/sdd/tier1-v3/` (`progress.md`, `CARRY.md` with rulings
D1-D8 and C-1, `task-<LANE>-report.md` per lane with exact resume commands). Owner focus: alpha for a US equity
long/short book, score window the last 6 years.

## Headline

- **Disk:** 49 GB reclaimed in `atx-db` (C: free 29 → 78 GB); lanes then landed ~4 GB of stages (now 54 GB free,
  part of it other sessions). Deleted: `warehouse.duckdb` (catalog tables archived to `data/archive/warehouse-v2`,
  4.2 MB), `.pytest_cache` (9.8 GB of stale templates), v2 research probe state, a duplicate TickerHistory3, FSDS
  zips (parsed copy kept), identity scratch. One deletion was wrong: `data/research/lake` held the consumer's default
  `--lake` snapshot (see Owner actions).
- **S0.1 (mega-alpha v7 chain):** Reg SHO with the NYSE family, panel v2 2018-2026 and the borrow proxy are built.
  Section 5 metrics and the lo1 aligned export were running at the stop; the consumer acceptance load has not run.
- **Platform (S1) is done:** stage registry (35 stages), `lake verify`, byte-identical `catalog.duckdb` (59 views,
  51 `_as_of` macros), registry-driven orchestrator, parity catalog + scorecard v0. Package names are
  `atx_db.stagelake` and `atx_db.parityscore` (the plan's `lake/` and `parity/` would shadow v2 modules).
- **Every other sprint is partial.** All nine lanes' code and fixture tests are committed; most full builds did not
  finish because the host ran out of commit memory (VS Code ~5.5 GB, Claude processes ~3.5 GB, another session's
  pytest runs, nine lanes): the guard stopped builds 30+ times at < 0.75 GB free commit.

## Gates by sprint

| sprint | done (measured) | not done |
|---|---|---|
| S0 | S0.2 committed; S0.3 scorecard v0 (370 rows, 324 not measured); S0.5 rulings D1-D8 | S0.1 metrics / export / acceptance (in flight); S0.4 `SOURCES.md` (sources are listed in each lane report) |
| S1 | S1.1-S1.4 PASS; S1.5 PASS (FRED H.10 FX 23 series, H.15, VIX, French; clocks from release calendars) | S1.1 third hash-identical rebuild (ftd needs a 0.6 GiB cap) |
| S2 | S2.2 listing dates 96.51% PASS; S2.3 ISIN 100%, 13F value mapping 28/30 quarters 2019+; S2.5 link table v3: PIT 93.8-94.9% of member_equity cells 2019-2025 (gate 95%), 2018 94.3%, 0 ambiguous | S2.1 cover page (data landed, build not run) → closes most of the S2.5 gap (foreign private issuers); S2.4 FIGI/LEI; S2.6 |
| S3 | S3.4 shares daily PASS (99.64% of cells; PIT two-source agreement 96.3%) | S3.7 liquidity (2/64 units), CRSP-style market returns + French checks, S3.6 index proxies, S3.2 (code done) |
| S4 | v10 code: cross-concept Q4/TTM, H.10 FX, catalog items, FSDS PRE label fallback, vintages (85 tests); samples: missing TTM sale/oi/gp 2.39/1.38/1.88% → 1.16/0.79/1.33% | full `fundamentals_v10` build, `fundamental-events-v2`, S4.7 validation, §2 targets; S4.4 notes build (Notes data sets 2019q1-2026_08 landed: 818 MB) |
| S5 | S5.5 Reg SHO complete PASS; insider net buying 136k issuer-months; 13D 2025 99.9% parsed | S5.1 filer type (inputs landed); S5.2 N-PORT (16/27 quarters parsed); 13G; 13D names/activism hand check FAIL (65/100, 17/29); Form 144 |
| S6 | `events/governance` (111,938) and `events/capital` (36,398) published, unchecked | guidance (first pass ~73% precise, fixes not re-measured), buybacks, M&A, calendar v2, hand checks |
| S7 | S7.1 FF 5-49 + approximate NAICS for 8,467/8,467 linked issuers PASS | S7.2 TNIC / S7.3 Lazy Prices (landing 2,286 of 32,411 filings); Loughran-McDonald skipped (licence); S7.4 not run |
| S8 | S8.1 PASS (6 adapters, 52 contract tests); S8.2 free `options/` term slope 99.6-99.9% of optionable cells | skew / option volume / OI need SpiderRock OptionEODFeaturesHist + SurfaceFixedGridHist (D3) |
| S9, S10 | — | not started (depend on S3/S4/S5/S7 builds) |

## Published stages usable now (lake root `atx-db/data/alpha_panel/v1`)

`panel/` v2 2018-2026, `regsho_threshold/` (all 5 markets), `borrow_proxy/`, `reference/` (fx_daily, rates, vix,
french), `classification/`, `market/market_shares*`, `identity/link_table_v3.parquet` + `export/identity-bridge-v3-*`,
`security_master/{listing_events,line_listing,name_history,cusip_history}`, `options/`, `insider_ext/`,
`events/{governance,capital}`, `notes/` (raw subsets). Stale or partial, do not bind: `stakes/` (old parser),
`classification_tnic/` and `text/` (test builds), `nport/parts`, `thirteenf_filer_type/` inputs (no manifests).

## Owner actions

1. **Stop leftover processes** (the permission classifier refused the lanes' kills; I did not kill on their behalf).
   All are resumable. TXT fetch loops: `Stop-Process -Id 2812,24936,11044,29004 -Force` then
   `Stop-Process -Id 20228,8584,19336,16360,7752,18888 -Force`. ID chain: `Stop-Process -Id 7456,19252,18780,22352,24328,24944,27652 -Force`.
   OWN 13D/G fetches: `Stop-Process -Id 17224,19884,21832,3996 -Force`. PIDs change on relaunch: check the command
   lines first.
2. **Restore the consumer research lake.** `prepare_research_fields.py --lake` defaults to
   `atx-db/data/research/lake/price-wave-0ed96b2696f1-5b596288cf23` (mktcap_lagged, size_grp, line types), which the
   disk batch deleted. Rebuild (TickerHistory3 only, no warehouse):
   `run_memory_guarded.py --job-gb 0.2 --allow-nested-guards -- python scripts/research_price_wave.py run --stages units,dups,ids,bars,lines,bench,spine,market,lake --th3 C:/Users/natha/Downloads/TickerHistory3.parquet --workers 1`.
   Two attempts failed (module shadowing, since fixed; then a host-memory stop). Fields prepared earlier are not
   affected; any new `prepare_research_fields` run asking for those fields fails until this is rebuilt.
3. **Decisions:** (a) S2.1 2020 gate: early cover-page ticker tagging was large filers only, so "≥ 95% of all
   10-K/10-Q filers" likely fails for 2020 as worded; rule the denominator (linked CIKs 89.5%, ever-member 93.5% in
   the trial). (b) ftd rebuild cap 0.6 GiB for the S1.1 third identity rebuild. (c) Switch the panel's lagged vendor
   `shares_out` to `market_shares.shrout`? (d) Merge `feat/tier1-v3-warehouse` to `main` (user gate).
4. **Run heavy builds with ≤ 4 concurrent data lanes** and nothing else heavy on the host (CARRY: C-1 revoked).

## Resume order for alpha value

1. S0.1 tail (if the in-flight run failed): `bash .superpowers/sdd/tier1-v3/receipts/s0.1-chain.sh` with
   `SKIP_MANIFEST=1 SKIP_BORROW=1`, then the consumer acceptance load (commands in
   `ALPHA_PANEL_REQUEST_V7_RESPONSE.md`, last section).
2. FUND: `fundamentals build-all` with `ATX_FUND_STAGE=fundamentals_v10`, validate, export v2 → swap into the panel
   (the largest §2 coverage lever; FX recovers ~5.5% of member cells' money items).
3. NOTES `cover_page build` → ID `identity_v3` rerun (S2.5 to ≥ 95%) → FIGI/LEI.
4. MKT `liquidity all`, `market_index build` (French checks ≤ 2022), `indexes build`.
5. OWN `filer_type build`, N-PORT remaining quarters + build; EVT guidance re-parse + hand check; TXT landing
   (verify phase first, one loop) → TNIC, Lazy Prices.
6. Then S9 (characteristics, OSAP replication; `data/raw/benchmarks/{osap,jkp,french}` are already landed) and S10.

## Open issues for consumers

- **IV clock.** The panel exports `iv_atm_*` (TickerHistory3 `atmCenI_*`) as visible the next session. SpiderRock
  documents delivery of this history at 22:00 CT (03:00-04:00 UTC the next day), which is after the 22:00 UTC d-1
  mark. Unless the live system computes the value itself at the close, the fields are one session early.
- `insider_ext` value sums are polluted by misreported Form 4 prices (needs a price-sanity rule); share counts are fine.
- Link table v3 `name` tier: two CIKs differ from SEC's current map (FSSL, CBAT: re-registered issuers).
