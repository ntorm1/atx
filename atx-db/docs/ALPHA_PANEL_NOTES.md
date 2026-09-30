# Alpha panel: SEC Financial Statement and Notes data sets (tier1-v3 S2.1, S4.4)

One landing serves two tasks:

- **S2.1**, cover-page identity evidence: `identity_cover/cover_page.parquet`;
- **S4.4**, segments and notes items: `fundamentals_notes/`.

Code lives in `src/atx_db/alpha_panel/`:

- `notes_fetch.py`: landing plus stage `notes/`;
- `cover_page.py`: stage `identity_cover/`;
- `fund_notes.py`: stage `fundamentals_notes/`.

Each module docstring holds the full rules. Every stage publishes its `manifest.json` last. The manifest carries the
schema, the code SHA-256 plus git HEAD, the SHA-256 of every output and of every input manifest, the rules, and the
build receipt.

Tests are fixture-only and offline, about 1 s each:

- `tests/test_alpha_panel_notes.py`: landing, parse and verification;
- `tests/test_alpha_panel_cover.py`: S2.1;
- `tests/test_alpha_panel_notes_fund.py`: S4.4.

## Source

| item | value |
|---|---|
| page | `https://www.sec.gov/data-research/sec-markets-data/financial-statement-notes-data-sets` |
| url pattern | `https://www.sec.gov/files/dera/data/financial-statement-notes-data-sets/{YYYYqN,YYYY_MM}_notes.zip` |
| cadence | quarterly `2009q1` .. `2025q2`, then monthly from `2025_07` (the page, 2026-09-29; the brief expected the switch after 2020q3) |
| landed | 40 data sets, `2019q1` .. `2026_08` (ruling D7: from 2019), 15,456,189,519 bytes (15.5 GB), downloaded at ~20-30 MB/s |
| content | every XBRL submission (all forms: 10-K/Q, 20-F, 40-F, 8-K cover pages, S-1, ...) accepted in the span: `sub/tag/dim/num/txt/ren/pre/cal.tsv` |
| terms | SEC public data; EDGAR fair-access policy (declared user agent; at most 10 req/s) |
| rate limit | every request goes through `atx_db.sec_http` (host-wide 5 req/s limiter, approved user agent) |
| receipts | `data/raw/sec_notes/receipts.jsonl` (copy: `data/archive/receipts/sec_notes-receipts.jsonl`): url, bytes, sha256, http_status, fetched_at, Last-Modified, ETag, member size/CRC/lines/rows, part rows/bytes/sha256, verification, peak memory |

The zips are deleted after parse. At most two are on disk at a time (one parsing, one prefetching), and C: must keep
40 GB free after each download.

## Stage `notes/` (landing, `notes_fetch.py`)

`fetch` streams `sub/dim/txt/num.tsv` straight out of each zip. Nothing is extracted. The pyarrow CSV parser reads
line-aligned 8 MiB chunks, one at a time; its own streaming reader over a Python file queued blocks without bound.
There is no DuckDB in this step, so it ran unguarded under controller ruling C-1. The measured peak over the 40 data
sets is 0.22 GiB of private commit (0.25 GiB working set). The process stops itself above 0.30 GiB.

Parts: `notes/parts/source=<key>/`. Rows are as served, with `''` as NULL, typed, plus `source`:

| part | rows (40 sets) | content |
|---|---:|---|
| `sub.parquet` | 650,412 | every submission; `accepted_et_naive` (EDGAR America/New_York wall clock, as published) and `accepted_utc` (DST-aware; the ambiguous fall-back hour resolves to EDT) |
| `txt_dei.parquet` | 16,350,527 | TXT rows of the dei taxonomy (cover page), all forms |
| `num_dei.parquet` | 305,722 | NUM rows of the dei taxonomy (share counts per class, public float), all forms |
| `num_items.parquet` | 50,152,652 | NUM rows of periodic forms needed by S4.4. These are standard tags matching `ITEM_TAG_RE` with any dimensions (segment measures and totals, debt, leases, pension, SBC, tax, goodwill/intangibles/impairments), plus every fact on a business-segment or geographic axis |
| `dim.parquet` | 4,211,282 | the DIM rows referenced by kept rows. `segments` is SEC's `Axis=Member;` string with the 'Statement', 'Axis', 'Member' and 'Domain' affixes stripped, e.g. `BusinessSegments=Americas;ConsolidationItems=OperatingSegments;` |

The parts total 818 MB.

**Checks on each zip** (all 40 pass):

- the zip SHA-256 equals the download receipt;
- per member, rows read + rows rejected = physical lines - 1;
- the SUB accession is unique;
- no kept row lacks a SUB row;
- every referenced dimension hash is present in DIM;
- no number, date or flag text fails to parse;
- every part re-reads with its written row count.

**Rejected rows.** The parser rejected 163 DIM rows in 20 data sets: segment strings with a stray field separator
(86 of them in `2026_06`). None of them is referenced by a kept row (`dim_missing` = 0). Their counts and samples are
in the receipts.

**Invariants of the source** (checked on all 650,412 SUB rows):

- every `filed` date lies inside its data set's span;
- no accession appears in two data sets.

The consumer stages therefore build per filing year and read only that year's data sets.

## Stage `identity_cover/` (S2.1, `cover_page.py`)

`cover_page.parquet` has one row per `(adsh, security_key, coreg)`:

- `security_key` is the DIM `segments` string of the cover fact's context, or `''` when the fact has no dimensions;
- a cover that lists several classes or securities gives several rows;
- a filing with no security-level fact keeps one row carrying its filing-level fields.

Every form is kept. 8-K covers carry the 12(b) table and date the ticker between periodic reports. Columns are
listed in the module docstring. The main ones:

| group | columns |
|---|---|
| filing | `cik, adsh, form, is_amendment, period, fy, fp, filed, accepted_utc, available_at, clock_basis, source_period, n_data_sets, vintage_risk` |
| security | `security_key, coreg, entity_cik, class_member, exchange_member, trading_symbol, symbol_norm, security_exchange_name, exchange_norm, security_12b_title, security_12g_title, no_trading_symbol_flag, is_equity_like, shares_outstanding, shares_outstanding_date` |
| filing level | `shares_outstanding_total, public_float, public_float_date, public_float_basis, auditor_name, auditor_location, auditor_firm_id, filer_category, inc_state_country, entity_registrant_name, entity_file_number, entity_tax_id, address_*, document_type, shell_company, small_business, emerging_growth, n_securities, instance_prefix` |

Rules:

- **Clock `cover-accepted-v1`.** `available_at` is the SUB acceptance time in UTC. When the acceptance time is
  missing, it is `filed 00:00 UTC + 46 h` (`clock_basis`). Nothing is backdated.
- **Facts.** One fact is kept per (accession, tag, context): the latest context end, then the lowest `iprx`.
- **Share-count date.** `shares_outstanding_date` is the reported cover date: `ddate - datp`, because SEC rounds
  `ddate` to month end.
- **Placeholders.** Symbols and exchanges such as 'N/A', 'NONE' or '-' become NULL in `symbol_norm` and
  `exchange_norm`.
- **Co-registrants.** A `LegalEntityAxis` context gets `coreg` and the co-registrant's own `EntityCentralIndexKey`
  as `entity_cik`.
- **`is_equity_like`.** A title/member heuristic: common, ordinary, ADS, LP units and beneficial interest count;
  preferred, depositary shares of preferred, warrants, rights, SPAC units and debt do not.
- **`instance_prefix`.** The `<prefix>-YYYYMMDD` stem of the XBRL instance file name, by EDGAR convention usually
  the ticker. It is heuristic evidence, reported separately and never merged into `trading_symbol`.

### Coverage (S2.1 done test)

**The full build has not run yet.** An owner stop came while it was queued behind the guard; see "Build state"
below.

The done criterion: >= 95% of 10-K/10-Q filers per year from 2020 have a dated ticker/exchange row. The measure is
`coverage.json`, per filing year: CIKs with a 10-K/10-Q (+T, +/A) that year, and the share with a real symbol or
exchange on (a) their own periodic cover or (b) any cover that year. It is also reported:

- without the CIKs that answer only `NoTradingSymbolFlag`;
- for the CIKs linked in `identity/link_table.parquet` that year (`linked`);
- for the CIKs whose line was ever a scorecard member (`member`);
- with `instance_prefix` as a fallback.

A partial rehearsal ran on a scratch copy of `2019q1`..`2021q2`, with the code before `exchange_norm` and
`instance_prefix` were added:

| filing year | periodic filer CIKs | own periodic cover | any cover | linked CIKs | any cover, linked | member CIKs | any cover, member |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2019 | 6,126 | 73.9% | 74.2% | 4,226 | 85.5% | 3,149 | 89.9% |
| 2020 | 6,045 | 68.0% | 68.3% | 4,348 | 89.5% | 3,287 | 93.5% |

The 2020 miss is the cover-tagging phase-in (see Limitations). Years from 2022 are expected near-complete for listed
filers, but they are not measured yet.

## Stage `fundamentals_notes/` (S4.4, `fund_notes.py`)

The inputs are `notes/` `sub`, `num_items` and `dim`. Each fact is read "as filed", once, at the lowest `iprx`, from
the data set that supplied its accession.

**Clock.** `available_at` is the SUB acceptance time in UTC, else `filed + 46 h`. An amendment is its own accession
with its own clock: a new row, never an overwrite.

Every row carries:

- `cik, adsh, form, period, fy, fp`;
- `tag, segments` (the dims), `value, uom` (unit or currency);
- `ddate` and `end_date` (`ddate - datp`), `qtrs`;
- `available_at`.

| output | content |
|---|---|
| `items/year=YYYY/items.parquet` | long table of the registry tags (`ITEMS`, `SEGMENT_METRICS`) with `item_group` (segment, debt, lease, pension, sbc, tax, impairment), `item`, `axis_kind` (business / geographic), `member`, `consolidation_member`. Notes families keep every dimensional breakdown (plan type, jurisdiction, instrument, ...). Segment metrics keep the total and the segment/geographic/consolidation-axis rows |
| `segments.parquet` | segment measures (revenue, operating_income, pretax_income, assets, long_lived_assets, capex, dda) on one business or geographic member, alone or with a `ConsolidationItemsAxis` member |
| `segment_recon.parquet` | per original 10-K/10-KT: members, revenue tag, segment sum S, tagged reconciling items E, consolidated revenue T, `rel_diff`, `recon_class` |
| `notes_wide.parquet` | one row per filing with the current-period value of every `ITEMS` item (below), in the filer currency; flows are year-to-date (`flow_qtrs`) |

**Wide items.** Instants are taken at `ddate = period`; flows over the filing's year-to-date quarters; all values
without dimensions, in the filer context, and from the first tag of each chain. The items:

- **debt**: `debt_mat_y1`..`debt_mat_y5`, `debt_mat_after5`, `debt_mat_y2_5`, `debt_mat_remainder_fy`, `debt_lt*`;
- **leases**: `oplease_*`, `finlease_*`, `lease_cost`, `rent_expense`;
- **pension**: `pension_pbo`, `pension_assets`, `pension_funded_status`, `pension_expense`, service / interest / return
  and contributions, discount rate, `dc_expense`. When no plan total is tagged, these fall back to the
  `PensionPlansDefinedBenefit` context (`pension_basis`);
- **SBC**: `sbc`, `sbc_expense`, `sbc_tax_benefit`, `sbc_unrecognized`;
- **tax**: `tax_total/current/deferred`, `tax_cur_{federal,state,foreign}`, `tax_def_{federal,state,foreign}`,
  `pretax_{domestic,foreign}`, effective and statutory rates, UTB, DTA/DTL, NOL;
- **impairment**: `goodwill`, `gw_impairment`, `intang_impairment` (else indefinite-lived + finite-lived),
  `gw_intang_impairment`, `asset_impairment`, `lla_impairment`.

**Reconciliation rule.** It applies to a multi-segment filing: at least 2 business-segment members that are not
reconciling-like (`RECONCILING_RE`: elimination, corporate, reconciling, unallocated, total, ...) over any fact of
the 10-K. The inputs:

- **S**: one revenue value per member, for the current fiscal year (`qtrs = 4`, `ddate = period`) in the filer
  currency. A segment-only context is preferred over a `ConsolidationItems=OperatingSegments` one; total-like members
  are dropped. The revenue chain is Revenues, RevenueFromContractWithCustomer(Ex/In)cludingAssessedTax,
  SalesRevenueNet, ...; the tag used is the first one whose consolidated total exists.
- **T**: the same tag without dimensions, else the first chain tag.
- **E**: tagged `ConsolidationItems` elimination / reconciling / corporate / other members with no segment member.

The classes:

- `direct`: |S - T| <= 1% |T|;
- `with_reconciling`: |S +/- E - T| <= 1%;
- otherwise one of `segments_exceed_total` (untagged intersegment eliminations), `segments_below_total` (an untagged
  "all other" / corporate line, or partial tagging), `double_count` (S >= 1.9 T), `scale_or_unit`, `no_total`,
  `zero_total`.

### Coverage (S4.4 done test)

**Not run.** An owner stop came before the build. The measure is `coverage.json`, per filing year:

- `multi_segment` 10-K count;
- `share_segment_revenue`: at least one business-segment revenue member for the current fiscal year (target >= 90%);
- `share_reconciled`: `direct` + `with_reconciling` among those (target >= 95%);
- `recon_classes`: the miss classes;
- `wide_nonnull_10k`: the non-null share of every notes item in 10-Ks.

## Build

```bash
cd C:/atx/atx-db && export PYTHONPATH=C:/atx/atx-db/src OPENBLAS_NUM_THREADS=1
# landing: pure pyarrow, no DuckDB -> unguarded per ruling C-1; batches of 4 data sets per process, resumable
.venv/Scripts/python.exe -m atx_db.alpha_panel.notes_fetch fetch --limit 4     # repeat until "0 to do"
.venv/Scripts/python.exe -m atx_db.alpha_panel.notes_fetch finalize           # re-hash parts, archive receipts, manifest
# consumers: DuckDB -> guarded (0.6 GiB cap, 300 MB DuckDB, per-year tables in a scratch db file)
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 240 -- \
    .venv/Scripts/python.exe -m atx_db.alpha_panel.cover_page build
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 0.6 --wait-minutes 240 -- \
    .venv/Scripts/python.exe -m atx_db.alpha_panel.fund_notes build
```

### Build state (2026-09-30)

- **Landing** (`notes/`): complete. 40/40 data sets, 2019q1-2026_08, 15.46 GB downloaded, 818 MB of parts kept.
  Parse time was 2,659 s in total; download 543 s. `notes/manifest.json` is published; the zips are deleted.
- **`identity_cover/` and `fundamentals_notes/`**: code and fixture tests done, not built. The guarded build was
  queued when the owner stop came and was cancelled before admission.
- **Resume**: the two guarded commands above. They need no network; each is expected to take a few minutes under the
  0.6 GiB cap. The per-year tables keep the DuckDB footprint to one filing year.

## Limitations

- **Coverage before mid-2021.** Cover-page tagging of the 12(b) table (`TradingSymbol`, `SecurityExchangeName`,
  `Security12bTitle`) phased in by filer size: large accelerated filers for periods after 2019-06-15, accelerated
  after 2020-06-15, all others after 2021-06-15. 2019-2020 covers of small filers carry no ticker. `instance_prefix`
  is the heuristic fallback.
- **Not every periodic filer has a listed security.** Debt-only registrants, non-traded REITs and BDCs, trusts and
  shells file 10-K/10-Q with an empty 12(b) table. Only some tag `NoTradingSymbolFlag`. The all-filer denominator
  therefore understates coverage of the traded-equity universe; the linked and member columns measure that universe.
- **SEC data-set conventions.** `ddate` is rounded to month end; `end_date` restores it through `datp`. DIM
  `segments` truncates at 1,024 characters (`segt`).
- **Values are "as filed".** Scale errors and sign conventions are the filer's. The segment reconciliation classes
  expose them rather than repair them.
- **Filer currency.** Notes values are in the filer's reporting currency (`uom`). FX conversion (ruling D4) belongs
  to the fundamentals engine.
- **iXBRL text blocks.** They are not kept. Only the dei rows of TXT are extracted (the S2.1 scope).
