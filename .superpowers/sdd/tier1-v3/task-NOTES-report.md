# task-NOTES report: S2.1 (cover-page identity evidence) + S4.4 (segments and notes items)

Lane NOTES, tier1-v3. The work stopped at the owner-stop request of 2026-09-30.

**Status.** The landing is complete. Both consumer stages are coded and tested, but the full builds were not run:
the guarded job was cancelled while still queued, and no data was touched.

## 1. What was built

| piece | path | state |
|---|---|---|
| landing + stage `notes/` | `atx-db/src/atx_db/alpha_panel/notes_fetch.py` | **done**: 40/40 data sets parsed, manifest published |
| S2.1 stage `identity_cover/` | `atx-db/src/atx_db/alpha_panel/cover_page.py` | code + tests done; full build **not run** |
| S4.4 stage `fundamentals_notes/` | `atx-db/src/atx_db/alpha_panel/fund_notes.py` | code + tests done; full build **not run** |
| tests | `atx-db/tests/test_alpha_panel_notes.py`, `test_alpha_panel_cover.py`, `test_alpha_panel_notes_fund.py` | 29 passed, ~1 s each, fixture-only |
| doc | `atx-db/docs/ALPHA_PANEL_NOTES.md` | written, including the build state and the resume commands |

**Stage `notes/`.** Path `atx-db/data/alpha_panel/v1/notes/`. `manifest.json` sha256 is
`783808a4f8222bb7fc3c7521fe2fb5a8e9963c0d91dc6706b57087e61ba34e5e`.

- Parts: `parts/source=<key>/{sub,txt_dei,num_dei,num_items,dim}.parquet`, 818 MB in total.
- Rows: sub 650,412; txt_dei 16,350,527; num_dei 305,722; num_items 50,152,652; dim 4,211,282.
- Every data set passed verification:
  - zip sha256 matches the receipt;
  - rows read + rows rejected = physical lines - 1, for every member;
  - no duplicate accession, no orphan rows, no missing dimensions;
  - no unparsable values.
- The parser rejected 163 DIM rows over 20 data sets. These are segment strings with a stray tab (86 in `2026_06`).
  None of them is referenced by a kept row.

**Runtime.** Pure pyarrow, no DuckDB: **unguarded per C-1**.

- Measured peak over the 40 data sets: **0.219 GiB private commit** (0.254 GiB working set, `2021q4`).
- The process stops itself above 0.30 GiB.
- Each process handled at most 4 data sets, so each wrote less than 150 MB.
- The first version used pyarrow's streaming CSV reader over a Python file. Its readahead queue grew without bound
  (0.16 GiB after 30 blocks), so it was replaced by synchronous line-aligned chunks.

## 2. Done criteria

| task | criterion | result | measured | command |
|---|---|---|---|---|
| landing | every data set from 2019q1 landed, parsed, verified, zip deleted, receipt kept | **PASS** | 40/40; `zips_on_disk: []`; ledger in raw + archive | `python -m atx_db.alpha_panel.notes_fetch status` / `finalize` |
| S2.1 | >= 95% of 10-K/10-Q filers per year from 2020 have a dated ticker/exchange row | **NOT RUN** (full build) | rehearsal on the 2019q1-2021q2 subset: 2020 all periodic filers 68.3%, linked CIKs 89.5%, member CIKs 93.5% | `run_memory_guarded.py --job-gb 0.6 -- python -m atx_db.alpha_panel.cover_page build` → `identity_cover/coverage.json` |
| S2.1 | `COVER READY` signal | **NOT DONE** | stage not published | — |
| S4.4 | segment revenue for >= 90% of multi-segment 10-K filers | **NOT RUN** | — | `run_memory_guarded.py --job-gb 0.6 -- python -m atx_db.alpha_panel.fund_notes build` → `fundamentals_notes/coverage.json` (`segments.*.share_segment_revenue`) |
| S4.4 | segment sum within 1% of consolidated revenue on >= 95%; miss classes reported | **NOT RUN** | classes are implemented and tested: `direct`, `with_reconciling`, `segments_exceed_total` (eliminations), `segments_below_total` ("all other" / corporate), `double_count`, `scale_or_unit`, `no_total` | same, `share_reconciled`, `recon_classes` |

**Expected risk on S2.1.** 2020 will likely **FAIL** the literal all-filer test:

- cover-page 12(b) tagging phased in by filer size, reaching non-accelerated filers only after 2021-06-15;
- many periodic filers (debt-only issuers, non-traded REITs/BDCs, trusts) have no listed security.

The coverage output reports several denominators so that the right one can be ruled on:

- all filers;
- all filers without the `NoTradingSymbolFlag`-only CIKs;
- linked CIKs;
- ever-member CIKs;
- each of these with the heuristic `instance_prefix` (the EDGAR instance stem, usually the ticker) as a fallback.

## 3. Sources

| item | value |
|---|---|
| source | SEC DERA *Financial Statement and Notes* data sets, page `https://www.sec.gov/data-research/sec-markets-data/financial-statement-notes-data-sets` |
| url pattern | `https://www.sec.gov/files/dera/data/financial-statement-notes-data-sets/{YYYYqN,YYYY_MM}_notes.zip` |
| cadence | quarterly to **2025q2**, monthly from **2025_07**. This is confirmed on the page; the brief expected the switch after 2020q3 |
| span landed | 2019q1 .. 2026_08 (40 files; ruling D7) |
| bytes landed | 15,456,189,519 (15.5 GB). Download 543 s at about 20-30 MB/s; parse 2,659 s |
| terms | SEC public data; EDGAR fair-access policy (declared user agent, <= 10 req/s) |
| rate limit | all traffic through `atx_db.sec_http` (host-wide 5 req/s, approved UA) |
| receipts | `atx-db/data/raw/sec_notes/receipts.jsonl`, copied to `atx-db/data/archive/receipts/sec_notes-receipts.jsonl` (sha256 `48826531…94dc`); page snapshots in `data/raw/sec_notes/page_*.html.gz`, per-set `notes-metadata.json.gz` in `data/raw/sec_notes/meta/` |

**Disk used.** 818 MB of parts plus 0.8 MB of receipts and metadata. All zips are deleted. C: had 53 GB free at the
stop.

## 4. Deviations and open issues

1. **Builds not run** (owner stop). To resume, run both guarded commands from `atx-db`. They need no network and
   read only `notes/`:
   ```bash
   cd C:/atx/atx-db && export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1
   .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 240 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.cover_page build
   .venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 240 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes build
   ```
   Then:
   - read `identity_cover/coverage.json` and `fundamentals_notes/coverage.json` into this table;
   - fill the two tables in `docs/ALPHA_PANEL_NOTES.md`;
   - append `COVER READY atx-db/data/alpha_panel/v1/identity_cover/cover_page.parquet <sha256>` to `signals.md`
     (lane ID waits on it; the schema was already sent to ID).
2. **Guard caps for the builds.** A single all-years DuckDB pass hit the 0.4 GiB cap and then the 0.6 GiB cap in the
   rehearsals. Both builds now work per filing year: tables live in a scratch db file, and yearly parts are stitched
   into the single output. A data set holds exactly its span's filings (checked on all 650k SUB rows).
   Cover on 2019-2021H1 ran in 35 s at 0.6 GiB. The per-year `fund_notes` build has not run on real data.
3. **Cadence and landing layout differ from the brief.** Monthly files start in 2025_07, not 2020q4. The planned
   `notes/` sub-stages became one directory per data set: `parts/source=<key>/{sub, txt_dei, num_dei, num_items, dim}`.
4. **Heuristic evidence.** `instance_prefix` (in S2.1) and `is_equity_like` are heuristics. Both are documented, and
   neither is merged into `trading_symbol`.
5. **Controller rules.** The later rule `git commit -m ... -- <paths>` is in use. The first commit (`a009dc13`) used
   explicit `git add` paths plus `-- <paths>` as well.
