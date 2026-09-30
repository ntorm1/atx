# task-TXT report (lane TXT: S7.2 text industries, S7.3 filing text features, S7.4 credit ratings)

Status: **stopped at the owner's request (2026-09-30 ~01:35Z) with the landing at 7% of scope.** Code, tests and
docs are complete and committed; both builds were proven on the landed data; the done criteria are not measured.

## 1. What was built

| item | path | state |
| --- | --- | --- |
| landing module | `atx-db/src/atx_db/alpha_panel/filing_text.py` | done: selection, fetch (host limiter, budget counter), TOC-aware Item parser (10-K 1/1A/7, 20-F 4/3.D/5), title fallback, EX-13 pass, `verify`, `status`, `--reland` |
| TNIC stage | `atx-db/src/atx_db/alpha_panel/tnic.py` -> `classification_tnic/` | done (code); test build only (2018 calibration) |
| text features stage | `atx-db/src/atx_db/alpha_panel/text_features.py` -> `text/` | done (code); test build only |
| tests | `atx-db/tests/test_alpha_panel_text.py` (24), `atx-db/tests/test_alpha_panel_tnic.py` (8) | 32 pass, 11 s, offline |
| doc | `atx-db/docs/ALPHA_PANEL_TEXT.md` | method, schemas, clocks, interim results, gaps |
| landing | `atx-db/data/raw/sec_text/{sections,docs}/part-*.parquet`, `receipts.jsonl` | 2,286 unique filings (2,600 docs rows incl. 314 duplicates from two concurrent loops) |

Landing scope (controller ruling 2026-09-30): CIKs linked (any tier) to a `member_equity` panel line 2019-2026 =
4,834 CIKs; original 10-K family + 20-F filed 2018+ = **32,411 filings** (priority 1 filed from 2018-08-01, newest
first: 29,505; priority 2 rest of 2018: 2,906). 40-F excluded (sections live in EX-99 exhibits). Landed in scope:
about 2,180 (1,580 of the 2018 Q1 filings measured in scope at 00:50Z plus 600 filings of 2026).

Parse quality on the landed filings (docs parts, parser v1+v2): 10-K (2,008) Item 1 99.7%, 1A 99.65%, 7 99.85%,
all three 99.35%; 20-F (278) Item 4 97.1%, 3.D 99.3%, 5 97.5%, all three 96.0%. Numbered-heading method for 99.0% of
sections, title fallback 1.0%; `by_reference` stubs: MD&A 39, 1A 6, Item 1 1 (EX-13 pass not yet run). Pilot hand
check (68 documents incl. AAPL, JPM, GE, WFC, BRK, TSLA, 12 20-Fs): every found section starts at the correct heading
and ends before the next item; GE (cross-reference index, no body Item headings) yields none (`index_only`); WFC
MD&A / 1A are incorporated from EX-13 (flagged); JPMorgan MD&A recovered in-document by title.

## 2. Done criteria

| criterion | status | measured | command |
| --- | --- | --- | --- |
| S7.2: >= 95% of linked 10-K filers per year 2019+ have a TNIC peer set (member basis) | NOT RUN | landing 7% (no 2019-2025 filings yet); TNIC test build: 2018 calibration only (1,641 docs, threshold 0.200 at SIC-3 pair share 2.83%, median nearest score 0.278; nothing published) | `python -m atx_db.alpha_panel.tnic` (guarded) |
| S7.3: >= 90% of member_equity cells 2020+ with a PIT value (400-day staleness) | NOT RUN | test build: 2,186 feature rows; 2019 member_equity cells with a fresh value 14.0% (2018 Q1 filings only), 2020: 0% | `python -m atx_db.alpha_panel.text_features --phase all` (guarded) |
| S7.3: Loughran-McDonald sentiment only if licensed | PASS (skipped, recorded) | sraf.nd.edu (2026-09-29): "free for use in academic research"; "for commercial licenses, please contact" the authors -> not produced | WebFetch of the dictionary page |
| S7.4 (optional): NRSRO 17g-7(b) ratings for >= 80% of rated linked issuers | NOT RUN (documented why) | files live on each NRSRO's own website (non-SEC hosts: S&P, Moody's, Fitch, KBRA, DBRS...), each needs the S0.4 terms check and owner X-gate approval; disclosure lags 12 months (issuer-paid) / 24 months (others) | - |
| SEC budget <= 60,000 | PASS so far | 3,303 requests (attempts) in receipts, incl. a 200-request upper bound for one batch killed before its receipts | `receipts.jsonl` |
| disk >= 40 GB free | PASS | C: 50 GB free at stop (other lanes); this lane: landing 130 MB, `_tmp/text_profiles` 75 MB, `_tmp/tnic_tokens.parquet` 5 MB | `df -h /c`, `du -sh` |

## 3. Sources

| source | url | bytes landed | history | cadence | terms | rate limit |
| --- | --- | --- | --- | --- | --- | --- |
| EDGAR primary documents (10-K family, 20-F) | `https://www.sec.gov/Archives/edgar/data/<cik>/<acc>/<primaryDocument>` | ~12 GB transferred decompressed (avg 4.7 MB per document); 130 MB kept (sections only, zstd) | filed 2018-01..2026-09 | annual per filer | SEC public data, approved agent `atx-db/0.1 atx-research@example.com` | host-wide 5 req/s via `atx_db.sec_http` (shared by all lanes; acquire waits 0.2-3.4 s measured) |
| EDGAR filing index + EX-13 (pending) | `.../<acc>-index.htm`, EX-13 documents | 0 | same | same | same | same |
| Loughran-McDonald Master Dictionary | https://sraf.nd.edu/loughranmcdonald-master-dictionary/ | not landed | - | - | academic use free; commercial license required | - |
| NRSRO 17g-7(b) XBRL histories | each NRSRO website (SEC guide: https://www.sec.gov/about/divisions-offices/office-credit-ratings/disclosure-of-credit-rating-histories) | not landed | ratings from 2012-06-15 | 12/24-month lag | per-NRSRO terms, X gate | - |

## 4. Deviations and open issues

1. **Two fetch loops are still running** (a TaskStop killed only the tool wrapper; my process kill was denied by the
   auto-mode classifier; the controller surfaced it to the owner). Since ~00:57Z each relaunch imports the committed
   reduced-scope code. PIDs at 01:35Z:
   * loop A: bash 2812 -> bash 24936 -> guard 20228 / 8584 -> fetch python 7752 / 18888 (`--threads 3`, running);
   * loop B: bash 11044 -> bash 29004 -> guard 19336 / 16360 (`--threads 5`, waiting for admission).
   Stop the top bash processes first so they do not relaunch: `Stop-Process -Id 2812,24936,11044,29004 -Force`, then
   `Stop-Process -Id 20228,8584,19336,16360,7752,18888 -Force`.
   Two loops risk a part-number collision (same `part-N` from both), torn receipt lines, and double requests.
2. The memory guard stopped the fetch 6+ times with `stop_reason = low_commit` (host free commit < 0.75 GiB; this
   job peaked at 0.25-0.34 GiB). Throughput 0.6-1.5 req/s under the shared SEC limiter: the remaining ~30,200
   filings need about 6-9 h plus ~1-2 k requests for the EX-13 pass.
3. The first 1,200 filings were parsed by parser v1 (missed "Item 1.A", "Item I", "Our Business", "Combined MD&A");
   `--reland` refetches the 14 affected ones.
4. 40-F filers not covered (sections are exhibits); GE-style cross-reference 10-Ks give no sections.
5. The `classification_tnic/` and `text/` manifests on disk are test builds on partial data: rebuild both.
6. Deferred to other lanes: none required. `docs/SOURCES.md` entry for EDGAR primary documents (controller).

## 5. Resume instructions

```bash
cd C:/atx/atx-db && export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1
G=../.superpowers/sdd/tier1-parity/run_memory_guarded.py
# 0. make sure no filing_text / fetch_loop.sh process is running (section 4.1)
# 1. integrity pass: unreadable parts set aside, stale .partial removed, duplicates / torn receipts / refetch queue
.venv/Scripts/python.exe $G --job-gb 0.3 --wait-minutes 60 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.filing_text --phase verify
# 2. ONE relaunch loop (resumes from parts + receipts; relaunches after guard stops 137/78)
THREADS=5 bash data/raw/sec_text/_logs/fetch_loop.sh fetch
# 3. EX-13 exhibits for by-reference sections
bash data/raw/sec_text/_logs/fetch_loop.sh exhibits
# 4. builds (each guarded)
.venv/Scripts/python.exe $G --job-gb 0.6 --wait-minutes 240 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.tnic
.venv/Scripts/python.exe $G --job-gb 0.6 --wait-minutes 240 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.text_features --phase all --years 2019-2026
```

The gates are then read from `classification_tnic/years.parquet` (`coverage_linked_with_peers` strict TNIC-3,
`coverage_linked_with_peer_set` incl. the flagged nearest-5 fallback; member-equity-linked basis) and from the
`text/manifest.json` `coverage.panel_member_equity.<year>.any_section` (and `any_similarity`).

## Commits

4b98da0d (modules + tests), f220d52e (parser v2, per-filing receipts, memory), 15d031a2 (member_equity scope),
5fce3d0e (verify, file-wise scope scan, TNIC member basis), and the final report commit.
