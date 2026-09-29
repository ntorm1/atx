# Alpha panel: status (2026-09-29, 00:50 UTC)

**Goal.** Deliver the mega-alpha v6.1 → v7 data request,
`C:\atx-wt\pool-2\docs\plans\2026-09-28-mega-alpha-data-request-atx-db.md`. The item-by-item answer is
[`ALPHA_PANEL_REQUEST_V7_RESPONSE.md`](ALPHA_PANEL_REQUEST_V7_RESPONSE.md).

**State: not finished.** Every source stage is built, validated and published with a SHA-bound manifest. The final
chain has not run:

- Reg SHO rebuild with the NYSE family;
- v2 panel assembly for 2018-2026;
- borrow proxy;
- section 5 metrics;
- the aligned export on the lo1 role;
- the consumer's acceptance load.

Nothing is committed.

**Still running.** One process: the NYSE combined Reg SHO landing (pid 11712, guard slot 0.25 GiB). It was started by
the stopped short-flow lane. It is resumable: `python -m atx_db.alpha_panel.regsho download --market
nyse_combined`. At 00:39 UTC it had landed 855 of about 2,180 dates, at about 15 dates a minute.

Build root: `atx-db/data/alpha_panel/v1` (git-ignored). Code: `atx-db/src/atx_db/alpha_panel/`. The stage chain in
dependency order is `build.py` `STAGES`.

## Stages

| stage | module | state | notes |
|---|---|---|---|
| prices, calendar | `prices.py` | done | 2018+ in `prices/`; 2012-03-26..2017 in `prices_history/` (D7) |
| identity tiers | `identity_links.py`, `identity_backfill.py`, `identity_names.py` | done | strict, backfill, name |
| U1 link table | `identity_table.py` | done | 172,926 runs; bridges `export/identity-bridge-v2-{strict,pit,all}` |
| U3 security master | `security_master.py` | done | FINRA names (PIT) plus the Nasdaq directory (non-PIT) |
| D10 corporate actions | `corporate_actions.py` | done | 262,560 vendor factor events |
| U4 delisting | `delisting.py` | done | 9,727 events; 85.8% of TRAIN member terminations classified; no vendor earnFlag |
| fundamentals v9 | `fundamentals.py`, `fund_items.py`, `fund_extract.py` | done, validated | 421,228 events; own-filing match 100%; cutoff rebuild 118/118 |
| fundamental-events export | `fund_export.py` | done, verified | `export/fundamental-events-v1`; loads through the consumer's `load_events` |
| short interest, short volume | `short_interest.py`, `short_volume.py` | done | |
| SEC filings, earnings calendar, Form 4 | `sec_filings.py`, `earnings_calendar.py`, `insider.py` | done | see `ALPHA_PANEL_SEC.md` |
| FTD | `ftd.py` | done (rebuilt 00:22 UTC with the sid0 repair) | |
| short volume splits | `short_volume_ext.py` | done | |
| 13F | `thirteenf.py` | done (aggregates rebuilt 00:31 on the new FTD map) | 124.4M holdings rows |
| Reg SHO | `regsho.py` | **rebuild pending** | published without the NYSE family (2018: 51 days only) |
| panel v2 | `panel.py` | **rebuild pending** | see below |
| borrow proxy | `borrow_proxy.py` | **not built** | needs the panel |
| metrics (section 5) | `metrics.py` | **not run** | needs the panel |
| lo1 aligned export | `export_impl.py align` | **not run** | needs the panel |

### Panel files now

| months | content |
|---|---|
| 2018-01 .. 2023-04 | v1-era assembly from 2026-09-27: vendor-earnFlag `member_equity`, no v2 columns |
| 2021-01, 2021-03, 2021-06 | v2 smoke months: every input joined, including 13F, FTD, Reg SHO, Form 4, earnings calendar |
| 2023-05 .. 2023-12 | missing |
| 2024, 2025, 2026 | stale single files from an earlier build |

The v2 `assemble` step rewrites every month and removes the old single-file layout. The core and membership
intermediates (`_tmp/panel_core`, `_tmp/panel_member`) are current and need no rebuild.

## Changes this session

- **Vendor earnFlag removed from every rule** (owner ruling). The replacements:
  - `member_equity`: SEC periodic filings and security type;
  - `earn_recent`: 8-K 2.02 reaction sessions;
  - delisting non-common: FINRA name types.

  The vendor columns stay as `*_vendor`, for audit only.
- **Panel v2 as-of sources**, each gated at `available_at < 22:00 UTC of d-1`, each with its own staleness:
  - filer regime;
  - FTD;
  - latest periodic report;
  - Form 4;
  - short-volume venues;
  - Reg SHO;
  - earnings calendar;
  - 13F (`inst_*`).
- **Panel manifest.** It now carries the per-file SHA-256, the code SHA and the SHA-256 of every input stage
  manifest. Year ranges can be assembled by separate runs; their receipts merge.
- **Metrics.**
  - New `linked_usd` basis. Money items of non-USD reporters are NaN by rule, because there is no FX source.
  - Return fields get no distribution for 2023+ windows (request section 6).
- **`build.py`.** The chain now includes the SEC, FTD, Reg SHO, 13F, borrow-proxy and export stages, with the panel
  split into `panel_core` and `panel` (assemble).
- **Docs.**
  - new: `ALPHA_PANEL_SHORTFLOW.md`, `ALPHA_PANEL_REQUEST_V7_RESPONSE.md`;
  - updated: `ALPHA_PANEL_IDENTITY_SECURITY.md` (U4 numbers, operating rule).
- **Tests.** All `tests/test_alpha_panel_*.py` pass.

## To finish

The commands are in the last section of `ALPHA_PANEL_REQUEST_V7_RESPONSE.md`. In order:
1. `regsho build`, once the landing has finished;
2. panel assemble for 2018-2021 and 2022-2026, which can run in parallel at 0.8 GiB each, followed by the manifest
   pass;
3. `borrow_proxy`;
4. `metrics`;
5. `export_impl align` on the lo1 role;
6. the consumer's `prepare_research_fields.py` acceptance load, with `export/identity-bridge-v2-pit` and
   `export/fundamental-events-v1`.

Allow about 1-1.5 h for step 2 and 30-45 min for steps 3-6. Then update the coverage tables in the response doc and
the identity doc from `ALPHA_PANEL_METRICS.md`, and commit with explicit pathspecs.

## Open issues and owner decisions

- **atx-engine seal.** Role sessions from 2025-01-01 are refused. The 2025-2026 data is built but can reach the book
  only if the owner lifts the seal.
- **FX for non-USD reporters.** They are 5.5% of member_equity cells, and their money items are NaN. A point-in-time
  FX source (FRED H.10) would recover most of them. This needs an owner decision.
- **TTM chain breaks.** 3.8-6.6% of 2020-2022 USD events have the quarter but no TTM. The main cause is a Q4 across a
  concept switch. The fix is cross-concept Q4 derivation in `fund_items.py`, which is not done.
- **13F filer type** is not classified: there is no free point-in-time label.
- **FPI earnings** are announced on 6-K and are missing from the earnings calendar.
- **ADR detection by name is weak**, because FINRA truncates names at 30 characters.
- **Not in any source:** GICS, NAICS, consensus, borrow fee and utilization, options skew / volume / OI, VWAP, trade
  count, index add/drop.

## Disk

Free space on C: is 45 GB.

**Deleted this session** (all in the atx-db data realm):
- `_tmp/prices_proj`, `_tmp/finra_th`, `_tmp/insider`;
- `fundamentals/_work` (573 MB of batch scratch);
- empty `.partial` leftovers;
- earlier in the session: the tier-1 `.bak` and other stale scratch.

**Kept:**
- raw source landings (`data/raw/sec_ftd`, `regsho_threshold`, `finra_*`), which are the provenance of published
  stages;
- `thirteenf/parts` (the D1 holdings deliverable);
- `_tmp/panel_core` and `_tmp/panel_member` (inputs to identity_table and delisting);
- `_tmp/shortflow` (1.3 GB): Reg SHO and borrow-proxy scratch; delete after those stages run.

**Candidates, not ours to delete:**
- `data/research/work/fundamentals/f1-s8-r2` (13 GB), tier-1 v2 session-8 probe state;
- `data/cache/P12-fsds` (5.3 GB), the FSDS zips behind `fsds_baseline.py`;
- `data/staging/broad-bars` (3.4 GB), the warehouse activation staging.
