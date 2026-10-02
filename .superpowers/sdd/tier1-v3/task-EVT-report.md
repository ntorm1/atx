# task-EVT report: events and calendars (S6.1 to S6.6), stopped at OWNER STOP

Lane EVT, branch `feat/tier1-v3-warehouse`. The owner stop came mid-run. At that point every fetch and parse loop
was stopped, the code was committed, and two tables were published (`governance`, `capital`). No hand check was
completed, so no precision criterion has been measured yet.

## Done criteria

| Task | Deliverable | Criterion | Status | Measured |
|---|---|---|---|---|
| S6.4 | `events/guidance.parquet` | >= 90% precision on 300 hand-checked releases | NOT RUN (not published) | Parse 1 (16,739 docs: 14,410 landed v2 + 2,329 fetched) gave 12,361 rows from 5,758 releases. I reviewed the first 33 of a 300-release sample (seed 20260929): about 20 of 75 rows were wrong, roughly 73% precision. Fixes are coded (below) but not yet re-measured. |
| S6.3 | `events/buyback.parquet` | 200-event hand check >= 95% | NOT RUN | Code and tests done. Docs about half fetched (see fetch table). Parse and publish not run. |
| S6.1 | `events/mna.parquet` | 200 deals >= 95%; consideration >= 80% | NOT RUN | Code and tests done. Merger and tender docs fetched (the batch1 list starts with them). Parse and publish not run. |
| S6.5 | `earnings_calendar_v2/` | >= 95% of member_equity issuers covered per year 2020+, FPIs included | NOT RUN | Code and tests done. FPI docs not fetched. Build not run. |
| S6.6 | `events/governance.parquet` | 200-event hand check >= 95% | PUBLISHED, check NOT RUN | 111,938 rows (per-year counts in the manifest). 200-event sample dumped (seed 7) but not reviewed. |
| S6.2 | `events/capital.parquet` | 200-event hand check >= 95% | PUBLISHED, check NOT RUN | 36,398 events. 200-event sample dumped (seed 11) but not reviewed. |

Governance originals 2019-2025, per year:
- 5.02 officer/director changes: about 11-12.8k per year.
  - CEO departure flag: 330-430 per year.
  - CFO departure flag: 234-400 per year.
- 4.01 auditor changes: 584-973.
- 4.02 non-reliance: 96-840 (the 2021 peak is the SPAC warrant restatements).
- Going concern (10-K): 795-1,309.

Capital events, 2024:

| Event type | Count |
|---|---|
| IPO | 224 (181 listed) |
| Follow-on | 261 |
| Shelf takedown | 4,236 |
| Secondary takedown | 161 |
| Convert pricing | 122 |
| Spin-off | 15 |

## Published stages

The events stage is `data/alpha_panel/v1/events/`; the manifest merges one `tables.<name>` entry per table.

| File | Rows | SHA-256 |
|---|---|---|
| `events/governance.parquet` | 111,938 | `8f2674b030cd457181a9c39800044be6890e65e14c89dcd769aa67c6070c57ff` |
| `events/capital.parquet` | 36,398 | `162ad3d29d4d149f40e2ba562854ff9ba2bf943a64057818f9c9a77bf8e3ff57` |
| `events/manifest.json` | n/a | `394b34fe4bce8f076e8db8ab19e87beb87774e2bb93d22b67f7307871d9afda2` |

Both tables were built guarded (0.6 GiB cap). Native peaks were 0.213 GiB (governance) and 0.187 GiB (capital).

Link basis:
- governance: 84,582 `on_date`, 2,601 `nearest_30d`, 24,755 `unlinked` (non-member filers).
- capital: 17,333 `on_date`, 310 `nearest_30d`, 18,755 `unlinked`.

## Built items

All code is in `atx-db/src/atx_db/alpha_panel/`:

- **`events_sources.py`**: EDGAR full-text-search harvester and document fetcher.
  - Threaded pages, a shared `sec_http` limiter, the lane request cap (`ATX_EVT_SEC_CAP`, 60,000), content-addressed landing, and a document catalog (DuckDB, guarded).
  - `document_text` HTML flattening.
  - `peak_memory`.
  - Registered queries:

    | Area | Query ids |
    |---|---|
    | Guidance, buyback, M&A, FPI | guidance, buyback, merger, tender, fpi_results, fpi_period_results |
    | Governance | ceo_depart_0-2, cfo_depart_0-2, going_concern |
    | Capital | convert_0-2, spinoff |

- **`events_plan.py`**: fetch plans (pure pyarrow). Plans: guidance_probe, guidance_follow, guidance, buyback, merger, tender, fpi.
- **`events_common.py`**: issuer-to-security link (`attach_security`: on-date line, else nearest line within 30 days, with `link_tier` / `link_basis`), plus `is_member_issuer` and a publish-last manifest per table.
- **`guidance.py`** (S6.4): rule-based EPS and revenue guidance extractor.
  - Phases: doclist (guarded), parse (C-1), publish (guarded).
  - Revisions per CIK.
  - Output columns match lane LIC's substitute: measure, period_type, period_end, low/high/mid, keyed on cik and available_at.
- **`events_buyback.py`** (S6.3): authorisation extractor.
  - Flags the first announcement within 180 days.
  - Joins the XBRL authorised / remaining amounts from `fundamentals/events.parquet` as-of before and after. These are joined, not recomputed.
- **`events_mna.py`** (S6.1):
  - Consideration parser: cash per share, stock ratio, CVR, election.
  - Filer role, and target/acquirer CIK resolution (co-filed 425 / SC TO-T, else an exact name match).
  - Status from metadata: completed (2.01 / 5.01 / 3.01, Form 25, delisting `mna`) or terminated (1.02).
  - Link to the delisting stage.
- **`earnings_calendar_v2.py`** (S6.5), a new stage; v1 is imported read-only:
  - v1 8-K 2.02 rows, plus classified 6-K results releases (period from text), plus a periodic-report fallback. The fallback is a 10-Q/10-K/20-F/40-F with no earlier announcement of the same period.
  - Timing, primaries and expected dates are recomputed with `earnings_calendar.process_cik`.
  - `member_coverage` per year from the panel's `member_equity` cells. Reported shares: overall, FPIs, announcement-only, and expected-count.
- **`events_governance.py`** (S6.6):
  - Events from items 5.02 / 4.01 / 4.02 and going-concern 10-Ks.
  - CEO/CFO departure flags from FTS phrase hits in the 8-K main document or EX-99 exhibits (EX-10 is excluded).
  - `dei:AuditorName` before/after is joined when `identity_cover/cover_page.parquet` is published. It is not published yet, so those columns are NULL (`auditor_basis` records this).
- **`events_capital.py`** (S6.2):
  - IPO: the first 424B4 after an S-1/F-1/S-11 filed within 540 days, with no line trading before; first session from `security_master/lines.parquet`; `is_spac` from SIC 6770.
  - Follow-on: any other 424B4.
  - Shelf takedown: 424B5; secondary shelf takedown: 424B7. Filings within 5 days are one event.
  - Convert pricing: 8-K FTS hits.
  - Spin-off: first Form 10-12B with distribution wording.
- **Tests** (all offline; 30 pass):
  - `test_alpha_panel_guidance.py` (13)
  - `test_alpha_panel_events_buyback.py` (5)
  - `test_alpha_panel_events_mna.py` (6)
  - `test_alpha_panel_events_governance.py` (2 fixture-lake builds)
  - `test_alpha_panel_events_calendar_v2.py` (4)

  Command: `PYTHONPATH=src .venv/Scripts/python.exe -m pytest tests/test_alpha_panel_guidance.py tests/test_alpha_panel_events_*.py -q`

Guidance fixes made after the 33-release review. They are in the committed code but not re-measured:
1. The historical-verb regex bug: `increased\s+\d\b` never matched "increased 23%". It now covers increased / decreased / grew / rose / fell / declined / improved followed by a number, plus "included".
2. A release title such as "Reports Q4 Results; Provides 2020 Outlook" no longer opens an outlook scope.
3. A sentence made forward-looking only by an outlook heading is rejected when it:
   - carries a historical cue, "in the quarter" or "year-to-date"; or
   - resolves an inferred (year-less) period.
4. Base values are dropped ("off the base of $7.70").
5. Component and reconciliation sentences are dropped ("difference between", "reconcil").
6. Revenue values are dropped when a change verb precedes the keyword without a linking word ("reduce total revenues $70 to $100 million"), or when the value is followed by "of <non-revenue noun>" ("$5.3 million of severance").
7. Words after "sales" that block the revenue keyword: representation, teams, agreement, channel, and similar.
8. "..., respectively" maps several values to several periods in order.

Known remaining error types:
- A pre-announcement of a just-ended year is labelled as the next year (Sherwin-Williams, January 2019).
- Outlook sub-headings ("Second Quarter 2019" under an outlook section) do not update the heading period.

## SEC requests and disk

- **SEC requests used by this lane: 17,103 of the 60,000 cap.**
  - 4,409 FTS pages (252 MB).
  - 12,694 Archives documents (4.35 GB uncompressed).
  - All HTTP 200.
  - Receipts: `atx-db/data/raw/sec_events/fetch-ledger.jsonl` (url, sha256, status, fetched_at, bytes, user_agent), plus `units-done.jsonl`.
- **Disk:**
  - `data/raw/sec_events` 349 MB (gzip objects).
  - `_tmp/events` 36 MB.
  - `events/` 2.7 MB.
  - C: had about 68 GB free when checked.

## Fetch state at stop

The loops are resumable: rerunning the same command skips URLs already in the ledger.

| URL list (`_tmp/events/`) | Size | Done at stop |
|---|---|---|
| `fetch_guidance_probe.txt` | 6,656 | complete (6,655 ok) |
| `fetch_evt_batch1.txt` (tender 233 + merger 4,565 + first 7,620 of buyback) | 12,418 | at least 4,000 (merger and tender are first, so they are complete) |
| `fetch_guidance_follow.txt` | 20,779 | at least 1,000 |
| `fetch_fpi.txt` | 12,138 | not started |

## Sources

- **EDGAR full-text search**: `https://efts.sec.gov/LATEST/search-index?q=...&forms=...&dateRange=custom&startdt=&enddt=&from=N`.
  - 100 hits per page, with a 10,000-hit ceiling per query window. Windows over 9,900 hits are halved.
  - Span 2019-01-01 to 2026-09-19 (ruling D7). One-off harvest.
  - SEC fair-access terms: at most 5 req/s shared host-wide through `atx_db.sec_http` with the approved user agent.
  - Harvested hits per query:

    | Query | Hits | Query | Hits |
    |---|---|---|---|
    | guidance | 221,596 | cfo_depart_0 | 2,245 |
    | fpi_results | 76,064 | cfo_depart_1 | 465 |
    | buyback | 45,191 | cfo_depart_2 | 382 |
    | fpi_period_results | 30,200 | going_concern | 8,686 |
    | merger | 13,344 | convert_0 | 2,142 |
    | tender | 2,081 | convert_1 | 114 |
    | ceo_depart_0 | 2,537 | convert_2 | 149 |
    | ceo_depart_1 | 959 | spinoff | 1,790 |
    | ceo_depart_2 | 947 | | |

- **EDGAR Archives**: `https://www.sec.gov/Archives/edgar/data/<cik>/<adsh>/<file>`. Same terms and limiter. Documents are capped at 8 MB.
- **Read-only lake inputs**:
  - `sec_filings` (filings, eight_k_items, issuer_profile)
  - `earnings_calendar` v1
  - `identity/link_table.parquet`
  - `security_master/lines.parquet`
  - `delisting/events.parquet`
  - `fundamentals/events.parquet`
  - panel `member_equity` cells (read only)
  - landed v2 earnings-release objects (`data/raw/sec-earnings-release`)

## Guard and C-1 record

Guarded runs, with native peaks:

| Job | Peak |
|---|---|
| events_sources catalog | 0.289 GiB |
| guidance doclist | 0.194 GiB |
| governance | 0.213 GiB |
| capital | 0.187 GiB |
| capital hand-check sampler | guarded |
| governance sampler rerun | guarded |

Run unguarded per C-1 (no DuckDB, network landing or pyarrow/stdlib transform writing < 200 MB), with measured peaks:

| Job | Peak |
|---|---|
| FTS harvests | 110 MB (measured on a guarded run) |
| fetch (probe) | 86 MB working set / 62 MB commit |
| events_plan | up to 265 MB working set / 247 MB commit (guidance_follow) |
| guidance parse | 149 MB working set / 194 MB commit, 2,445 s |

**Deviation:** the first governance hand-check sampler (a scratch script) ran unguarded and peaked at **1.75 GB working set**. The cause was pyarrow's dataset scanner readahead over `filings.parquet`. I changed it to `ParquetFile.iter_batches(batch_size=50,000)` and ran every later sampler under the guard.

## Resume instructions

Run from `C:/atx/atx-db` with `PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1`. `G` is the guard prefix:

```
G = .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 120 --
```

1. Resume the fetches (C-1, unguarded, resumable). Keep the total ledger under 60,000 lines; about 43k remain:
   - `python -m atx_db.alpha_panel.events_sources fetch data/alpha_panel/v1/_tmp/events/fetch_evt_batch1.txt`
   - `... fetch data/alpha_panel/v1/_tmp/events/fetch_guidance_follow.txt`
   - then `fetch_fpi.txt`, if budget allows (`--limit N`).
2. Rebuild the catalog: `G python -m atx_db.alpha_panel.events_sources catalog`.
3. Guidance:
   1. `G python -m atx_db.alpha_panel.guidance --phase doclist`
   2. `python -m atx_db.alpha_panel.guidance --phase parse` (C-1, about 40 min)
   3. Hand-check 300 releases: a random sample of accessions from `_tmp/events/guidance_rows.parquet`. Per row, judge measure, value, basis, period, and whether it is forward guidance. Record the cases in the report.
   4. `G python -m atx_db.alpha_panel.guidance --phase publish`
   5. Ping lane LIC.
4. Buyback: `python -m atx_db.alpha_panel.events_buyback --phase parse` (C-1), then `G ... --phase publish`. Then hand-check 200 `first_announcement` rows.
5. M&A: `python -m atx_db.alpha_panel.events_mna --phase parse`, then `G ... --phase publish`. Then hand-check 200 deals and read `consideration_share` from the manifest.
6. Calendar v2: `python -m atx_db.alpha_panel.earnings_calendar_v2 --phase parse` (after the FPI fetch), then `G ... --phase build`. The criterion is `member_coverage[year].covered_share` for 2020+ in `earnings_calendar_v2/manifest.json`.
7. Governance and capital hand checks: sample 200 events per table, stratified by event type (governance: CEO-flag 45, CFO-flag 40, both 5, unflagged 5.02 30, 4.01 30, 4.02 25, going concern 25; capital: IPO 50, follow-on 35, shelf 40, secondary 25, convert 35, spin-off 15). Fetch each event's evidence document (the FTS hit file for flagged or text rows, else `filings.primary_document`) and read the cue snippet. The dumps (`hc_gov.txt`, `hc_cap.txt`) were in the session scratchpad and are not kept.
8. Write `atx-db/docs/ALPHA_PANEL_EVENTS.md` with the measured numbers, and commit with `git commit -m ... -- <paths>`.

## Open issues

- CEO/CFO departure flags have limited recall. EDGAR FTS does not stem, and phrases such as "resignation as the Company's Chief Executive Officer" do not match the adjacent-phrase queries. The flag's precision is untested.
- Capital events do not classify security type (common vs debt) for 424B5, because a 424B5 includes the base prospectus.
- `identity_cover` (lane NOTES) is not published, so the auditor-name join is empty.
- Guidance cannot meet the 90% criterion without the fixes above. They need re-measurement after the full re-parse.
- Lane LIC waits on `events/guidance.parquet`, which is not published.
