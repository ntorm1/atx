# Alpha panel: filing text, text industries (TNIC) and Lazy Prices features (lane TXT, S7.2 / S7.3)

Plan: `docs/superpowers/plans/2026-09-28-tier1-v3-parity-warehouse.md` S7. Code:

| module | role |
| --- | --- |
| `src/atx_db/alpha_panel/filing_text.py` | landing: 10-K / 20-F primary documents -> Item sections (never the full filing) |
| `src/atx_db/alpha_panel/tnic.py` | stage `classification_tnic/`: Hoberg-Phillips TNIC-style peers from Item 1 |
| `src/atx_db/alpha_panel/text_features.py` | stage `text/`: Lazy Prices similarity, length, readability per filing |

Tests: `tests/test_alpha_panel_text.py` (parser on tricky fixtures, landing receipts/parts/resume, features, PIT
coverage), `tests/test_alpha_panel_tnic.py` (bitset cosine vs brute force, calibration, fallback, end-to-end build).

## Build

```bash
cd C:/atx/atx-db && export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1
G="../.superpowers/sdd/tier1-parity/run_memory_guarded.py"
.venv/Scripts/python.exe $G --job-gb 0.5 --wait-minutes 720 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.filing_text --phase fetch --threads 3     # network, resumable
.venv/Scripts/python.exe $G --job-gb 0.4 --wait-minutes 240 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.filing_text --phase exhibits --threads 3  # EX-13, network, resumable
.venv/Scripts/python.exe -m atx_db.alpha_panel.filing_text --phase status                                                                                # counts, budget, disk
.venv/Scripts/python.exe $G --job-gb 0.6 --wait-minutes 240 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.tnic
.venv/Scripts/python.exe $G --job-gb 0.6 --wait-minutes 240 -- .venv/Scripts/python.exe -m atx_db.alpha_panel.text_features --phase all
```

`ATX_SEC_TEXT_ROOT` overrides the landing root (`data/raw/sec_text`), `ATX_ALPHA_PANEL_ROOT` the stage lake.

## Landing `data/raw/sec_text/` (source: EDGAR archives)

Selection (`select_filings`): original 10-K family filings (`10-K`, `10-K405`, `10-KSB`, `10-KSB40`, `10-KT`,
`10-KT405`) filed 2018-01-01 or later by any CIK of `identity/link_table.parquet`, plus 20-F filings of CIKs with an
`ever_member` line; one row per accession (co-registrants in `ciks`). Priority 1 = CIKs with a member line, then
the other linked CIKs; oldest first. 10-K/A amendments are not landed (Part III only in almost all cases).

Per filing: one GET of `https://www.sec.gov/Archives/edgar/data/<cik>/<acc>/<primaryDocument>` through
`atx_db.sec_http` (approved agent, host-wide 5 req/s limiter shared with every lane, shared 403/429 pause),
responses above 50 MB refused (`response_too_large`). The document is converted to text and parsed in memory;
only the sections are written:

* `sections/part-NNNNN.parquet` (zstd level 6): `accession, cik, ciks, form, filing_date, report_date,
  available_at, section, item, method, flags, n_chars, source, parser, text`. `section` is `business` (10-K Item
  1, 20-F Item 4), `risk` (10-K Item 1A, 20-F Item 3.D) or `mdna` (10-K Item 7, 10-KSB Item 6, 20-F Item 5).
* `docs/part-NNNNN.parquet`: one row per landed filing with the clock, url, bytes, sha256, fetched_at, requests,
  whole-document text size, words, sentences, complex words and Fog (the full document is not kept, so its
  readability is measured here), parse diagnostics (`headings`, `toc_headings`, `running_lines`, `index_only`,
  `sections_found`).
* `exhibits/part-NNNNN.parquet`: the EX-13 pass (index url, exhibit urls, bytes, sha256, sections found).
* `receipts.jsonl`: one line per request (`kind` primary / index / ex13, url, bytes, sha256, http_status,
  fetched_at, requests (attempts incl. retries), error, terminal). Receipts are written before the batch's parts;
  a filing is done once its accession is in a `docs` part or it has a terminal non-200 receipt (4xx other than
  403/429, or `response_too_large`). The request budget (60,000) counts every attempt in the receipts.

### Parser (`html_to_text`, `find_items`, `extract_sections`)

* HTML to text on bytes: drop the inline-XBRL header (hidden facts), scripts, styles, comments, images; drop a
  table whose digits exceed 15% of its digits + letters (financial tables; LM-style); keep text tables one row per
  line; block elements become lines; inline tags are removed without a space (as rendered); entities unescaped;
  typographic quotes and dashes normalised; page-number and "Table of Contents" back-link lines dropped. UTF-8, else
  Windows-1252.
* Item headings: a line starting `Item N[L]` (optionally `Part II,`), the number and title possibly split over the
  next lines (`Item` / `1. Business`, `Item 5. Operating` / `and Financial Review`), whose title matches the item's
  expected title (so "Item 7 of this report ..." is not a heading). 20-F lettered sub-items (3.D) belong to their
  item.
* Table of contents: a run of 7+ distinct items, each within 400 characters of the next and in item order, whose items
  mostly reappear later. Such headings are ignored. (Short body items packed together, like a small filer's Items
  9B-15, do not reappear and stay headings.)
* A section runs from its heading to the next heading of a later item; among repeated headings (running page
  headers) the one giving the longest section wins, preferring a section that does not run to the end of the
  document.
* Fallback by title (`method = 'title'`): when no numbered heading survives, or the numbered section is shorter than
  300 characters, ends at the end of the document, or says the content is elsewhere ("incorporated by reference",
  "appears on pages", ...), a line that is exactly the section title (e.g. "Management's discussion and analysis",
  JPMorgan's in-document MD&A) starts the section, which ends at the next heading or known section title
  (`OTHER_TITLES`, e.g. "Consolidated balance sheets", "Report of independent registered public accounting firm").
* A cross-reference index entry (`Item 1A. Risk Factors 24-31`, GE) is not a section (`index_only`).
* Sections that overlap are cut at the later section's start; running headers/footers (short lines repeated 4+
  times, digits masked, e.g. `Apple Inc. | 2024 Form 10-K | 26`) are removed from section text.
* 20-F risk factors: from the `D. Risk Factors` sub-heading inside Item 3 (`flags: item3d`), else the whole Item 3
  (`item3_whole`).
* Flags: `by_reference` (short section pointing elsewhere), `eof`, `cut_at_<section>`, `ex13`.

### EX-13 pass (`--phase exhibits`)

For 10-K family filings with a `by_reference` section (bank holding companies incorporate MD&A and risk factors
from the annual report to shareholders): the filing index `<acc>-index.htm`, then up to 4 documents typed
`EX-13*`, converted and searched by title for the wanted sections (`source = 'ex13'`). The features and TNIC prefer
the exhibit over the by-reference stub.

## Stage `classification_tnic/` (S7.2)

See the `tnic.py` docstring for the method. Outputs:

* `pairs/year=YYYY.parquet`: `year, cik, peer_cik, score, accession, peer_accession, available_at, threshold,
  peer_basis` (both directions). `peer_basis = 'tnic3'`: score above the year's threshold, `available_at` = the
  later of the two 10-K acceptances. `peer_basis = 'nearest5_fallback'`: a firm with no TNIC-3 peer gets its 5
  nearest neighbours (score > 0); that set can change until the year's last filing, so `available_at` = the latest
  acceptance of the year (flagged, never mixed into TNIC-3).
* `firms.parquet`: `year, cik, accession, filing_date, filing_available_at, vocabulary_words, n_peers,
  n_peers_own_clock, n_fallback_peers, total_similarity, best_score, threshold, available_at, vintage_risk`.
* `years.parquet`: threshold, calibration year, SIC-3 pair share, vocabulary, coverage per year.

Consumer rule: the peer set of year `Y` is used from the pair's `available_at` (< 22:00 UTC of session d-1) until
the next year's set for the same firm is visible; at most 550 days.

## Stage `text/` (S7.3)

`features.parquet`: see the `text_features.py` docstring. One row per (cik, landed filing): clocks, the compared
prior filing (`prior_accession`, `prior_filing_date`, `prior_gap_days`), whole-document `doc_bytes` (Loughran-
McDonald 2014 file-size readability), `doc_words`, `doc_fog`, and per section (`business`, `risk`, `mdna`):
`_source, _method, _chars, _words, _sentences, _fog, _pct_complex, _sim_cosine, _sim_jaccard, _sim_minedit,
_words_chg`, plus `n_sections`. Consumer rule: as-of join on `cik` by `available_at` (< 22:00 UTC of session d-1),
stale after 400 days.

Sentiment: not produced. The Loughran-McDonald Master Dictionary page (sraf.nd.edu, checked 2026-09-29) says the
lists "are free for use in academic research" and "for commercial licenses, please contact" the authors; this
warehouse feeds a proprietary strategy, so the dictionary would need a commercial license (a D3 / S8 purchase
decision).

## Results (interim, 2026-09-30 ~01:35Z: landing stopped at the owner's request)

Landing scope (controller ruling 2026-09-30): 4,834 CIKs linked to a member_equity line 2019-2026; 32,411 filings
(10-K family 28,517, 20-F 3,900; priority 1 = filed from 2018-08-01: 29,505; priority 2 = rest of 2018: 2,906).

| measure | value |
| --- | --- |
| filings landed (unique accessions) | 2,286 (2,008 10-K, 278 20-F); 2018-01..03 (earlier scope) and 2026 (reduced scope, newest first) |
| 10-K sections found | Item 1 99.7%, 1A 99.65%, 7 99.85%, all three 99.35% |
| 20-F sections found | Item 4 97.1%, 3.D 99.3%, 5 97.5%, all three 96.0% |
| section method | numbered heading 99.0%, title fallback 1.0%; `by_reference` stubs: MD&A 39, 1A 6, Item 1 1 (EX-13 pass pending) |
| SEC requests (receipts, attempts) | 3,303 of the 60,000 budget |
| disk | landing 130 MB; `_tmp/text_profiles` 75 MB; stages < 1 MB |

Test builds on the landed data (guarded): `tnic` 2018 calibration: 1,641 Item 1 documents, vocabulary 33,907 after
the prior-year cap, threshold 0.200 at a 2.83% SIC-3 pair share (HP's published TNIC-3 cut is about 0.21), median
nearest-neighbour score 0.278, 45 s, 0.41 GiB peak. `text_features --phase all`: 7,444 section profiles, 2,186
feature rows, coverage path exercised (2019 member_equity cells with a fresh value: 14.0% from the 2018 Q1
filings alone). Both stages must be rebuilt after the landing completes.

## Gaps and risks

1. Landing incomplete (7% of the scope); the done criteria are not measured yet. Resume: `--phase verify`, then
   one `fetch_loop.sh`, then `--phase exhibits`, then `tnic` and `text_features --phase all`.
2. Cross-reference 10-Ks (GE: Items mapped to page ranges, no Item headings in the body) yield no sections
   (`index_only`); in-document MD&A references (JPMorgan) are recovered by title.
3. 40-F filers are not landed: their Business / MD&A are EX-99 exhibits (a 40-F pass would need the index plus 2-3
   exhibits per filing).
4. Readability uses a vowel-group syllable heuristic (e.g. "created" counts 1 syllable); Fog is comparable across
   filings, not to dictionary-based Fog.
5. TNIC keeps all non-stop, non-geographic words (no noun dictionary) and calibrates on the 2026 SIC snapshot.
6. Two duplicate fetch loops ran concurrently (a TaskStop left the bash loop alive); duplicate rows are harmless for
   the builds (dedupe by accession/section), and `verify` sizes them.
