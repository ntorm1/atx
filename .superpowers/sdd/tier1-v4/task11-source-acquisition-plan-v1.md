# Task11 retained 13F source acquisition plan v1

Status: planning only; no network/source acquisition authorized. This plan does
not certify bronze completeness, a source rule, economics, or historical market
population. It uses static code and controller-generated audit metadata only.
No foreign producer was invoked, edited, or imported. Native round5 retains
ownership of its producer/tests/docs/report; this file is the only changed path.

## Evidence and unresolved facts

| Item | Evidence available | Limit |
| --- | --- | --- |
| Official catalog endpoint | Both code paths name `https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets` | Page contents/current links have not been verified here |
| ZIP endpoint prefix | `atx-db/src/atx_db/thirteenf.py:54` discovers `https://www.sec.gov/files/structureddata/data/form-13f-data-sets/*_form13f.zip` | A constructed URL is a candidate, not proof of existence/content |
| Quarterly convention | Foreign `archive_from_url`, legacy alpha-panel `archive_period` parse `YYYYq[1-4]_form13f.zip` | Naming dates are filing windows, not holdings report quarters |
| Ranged convention | Those readers parse `DDmonYYYY-DDmonYYYY_form13f.zip` | Actual listed ranges must be retained; do not synthesize future ranges |
| ZIP layouts | Foreign `_archive_member_names` accepts required TSV basenames at ZIP root or in a period directory; duplicate required basename fails | Comment says wrapping began in2025; actual bytes/member layouts unverified |
| Retained roots | Controller auditv2 inspected repository/package/configured roots; named original ZIP count is0 in the generated all-file inventory | Opaque archives/alternate historical overrides are not disproved |
| Parsed source partitions | Generated file inventory contains54 `thirteenf/parts/source=*/holdings.parquet` paths;31 are2019+ through August2026 | Parsed outputs do not replace retained originals or prove complete partitions |
| Expected inventory source | Retained submissions ZIP archive SHA `702fbcd8b4335bc649e9e4eab3a202f3effc314b43421664bfecb59365767165` was verified by root; directory contains985667 main records | Archive retrieval clock is NULL; complete expected inventory has not been read/reconciled |
| First retrospective partition | Root part0000:10000 main+1328 history,7336 13F filing records, last `CIK0000731762.json`, one oversized `CIK0000019617.json` exception | UNQUALIFIED_PARTITION, bound reached; no entire-cohort claim |
| Other raw-store leads | Corrected root ledger auditv3:0 known13F,55536 UNKNOWN records,0exceptions, both scans complete | Opaque URLs remain UNKNOWN; zero leads is not bronze-absence proof |

Controller root environment overrides ATX_DB_DATA_DIR/ATX_DB_PATH/
ATX_ALPHA_PANEL_ROOT/ATX_EVT_RAW_DIR were absent during the audit. Foreign code
also supports custom fetch/cache/extraction paths. Historical overrides remain
unresolved until explicitly identified/audited; do not traverse an entire drive
or infer absence outside inspected roots.

Metadata sources: `task11-source-root-audit-v2.json`,
`task11-source-files-v2.jsonl`, submissions cache audit and
`task11-submissions-inventory-part0000-v1.jsonl.receipt.json`. The31 period names
below were read from generated metadata, not underlying parsed/raw source files.

## Can official bulk ZIPs replace individual filing downloads?

They are a plausible, substantially smaller acquisition path for original
**official as-served bulk table evidence**. Retain the exact ZIP bytes and every
member, rather than only four parsed tables. A ZIP plus authenticated member
records can establish reproducible SEC-distributed cover/summary/holding rows,
including accessions, if the official catalog/readme confirms the scope and
independent expected-accession reconciliation closes every gap.

This is not yet permission to treat a derived SEC bulk dataset as the exact
original EDGAR submission document. The controller must separately decide
whether official as-served bulk bronze satisfies the original-source contract
for source-supported fields, after inspecting retained SEC scope/readme/schema
documents. Full textual originals, omitted fields, notice relationship details,
confidential-treatment history, acceptance headers or correction provenance may
still require accession-specific originals. Do not set `verified_original`
from a successful ZIP download, CRC, parsed table, or reconciliation boolean.

Proposed admission types remain explicit:

- `official_bulk_archive`: retained SEC ZIP and authenticated table/member rows;
  only fields supported by verified documentary scope can be admitted.
- `edgar_submission_original`: retained full submission/primary documents for
  fields or exceptions that the bulk source cannot establish.
- `submission_inventory_clock`: independently retained submissions member/public
  acceptance evidence; it is neither a holding table nor economic completeness.

Use the bulk path first for cost efficiency if its contract is approved, then
request targeted originals only for independently counted unresolved accessions.
No thousands-of-filings fallback is preauthorized or included in this budget.

## Proposed deterministic URL inventory

Fix the first planning window to filing dates2019-01-01..2026-09-30, providing
2019 warmup and completed2020+ report cohorts. Latest completed report quarter
at that date is2026Q2;2026Q3 must not be treated as fully reported. This is a
planning cutoff, not a statement that sources already cover it.

Base: `https://www.sec.gov/files/structureddata/data/form-13f-data-sets/`.
Append `<period>_form13f.zip`. Candidate periods from existing metadata:

1. `{year}q{quarter}` for year2019..2023, quarter1..4:20 candidate URLs.
2. These11 ranged periods: `01jan2024-29feb2024`,
   `01mar2024-31may2024`, `01jun2024-31aug2024`,
   `01sep2024-30nov2024`, `01dec2024-28feb2025`,
   `01mar2025-31may2025`, `01jun2025-31aug2025`,
   `01sep2025-30nov2025`, `01dec2025-28feb2026`,
   `01mar2026-31may2026`, `01jun2026-31aug2026`.

Example candidates:
`https://www.sec.gov/files/structureddata/data/form-13f-data-sets/2019q1_form13f.zip`
and `https://www.sec.gov/files/structureddata/data/form-13f-data-sets/01jun2026-31aug2026_form13f.zip`.

These31 names are exact code/metadata-derived candidates, not currently verified
URL endpoints. Before ZIP acquisition, retain one official catalog response,
resolve linked URLs, validate SEC HTTPS host/path and canonical names, then freeze
the exact selected URL manifest. Acquire only catalog-confirmed URLs; do not
probe guessed names. Deduplicate identical endpoints without discarding archive
revision identities. Cap the selected ZIP URL inventory at64. Extra/missing/
overlapping/gapped intervals are explicit exceptions requiring a revised plan.

The observed31 windows end2026-08-31: September filing dates, including late2026Q2
reports/amendments/notices, are a named unresolved tail. If the official catalog
lists another published interval, include it with its actual stated dates. If
not, full recovery through September remains incomplete until publication or a
separately approved targeted-original plan. Do not silently claim quarter
completion using only timely arrivals. A filing window may contain earlier
report-quarter amendments; reconcile actual report dates rather than filename
quarters. Later archives may be necessary to close retrospective late reports;
those later observations never backfill earlier feature qualification/clocks.

## Retain all relevant members and raw fields

The foreign required-four-member contract is insufficient for full Task11
ownership admission. Preserve the entire official ZIP and central directory,
all member names/sizes/compression/CRC, readme/schema members and every raw field,
even when a current parser does not consume them.

| Member/field family | Static reader evidence | Admission work still required |
| --- | --- | --- |
| SUBMISSION | ACCESSION_NUMBER, FILING_DATE, SUBMISSIONTYPE, CIK, PERIODOFREPORT | Expected HR/HR-A/NT/NT-A set equality, report-date consistency, accession provenance; actual public acceptance clock is absent from this static selection |
| COVERPAGE | reportCalendarOrQuarter, amendment flag/number/type, manager name/address, REPORTTYPE, Form13F/CRD/SEC identifiers | Actual report type/notice/combination interpretation; no globally latest or deadline-backdated amendments |
| SUMMARYPAGE | OTHERINCLUDEDMANAGERSCOUNT, TABLEENTRYTOTAL, TABLEVALUETOTAL, ISCONFIDENTIALOMITTED | Full count/raw value reconciliation, confidentiality/omission history; public omitted amounts remain unknown |
| INFOTABLE | accession/table row key, issuer/class/CUSIP/FIGI, VALUE, SSHPRNAMT/TYPE, PUTCALL, discretion, OTHERMANAGER, sole/shared/none voting amounts | SH vs PRN/options, finite/nonnegative fields, mapping/class/split proof, unit regime, duplicate/overlap/shared-manager economic scope |
| OTHERMANAGER2 | Filer code reads accession, sequence, CIK, Form13F/CRD/SEC identifiers, name | Full raw included-manager relationships and count reconciliation; neither names nor CRDs alone prove no double count |
| OTHERMANAGER1, SIGNATURE and other members | Not required/selected by the foreign four-table importer | Actual presence/schema/scope unverified; preserve and inspect; absent necessary relationship/document evidence becomes a keyed gap |

Notices with no holdings table are expected filings, not known economic zeros.
Shared/discretion reports, combination reports and included-manager positions
require an explicit graph/relationship resolution; do not add two reports for
the same beneficial position merely because both appear in the bulk source.
Confidentiality cannot exempt retention of the public filing and public cover.
RESTATEMENT/NEW HOLDINGS and overlapping positions require actual version/row
lineage; retain prior public vintages and the amended rows before normalization.

Static legacy code references a filing-date value regime changing2023-01-03,
while the foreign warehouse importer uses a report-date threshold in at least
one conversion. These are conflicting implementation rules, not approved
documentary proofs. Preserve raw VALUE/TABLEVALUETOTAL, retain the official
readme/unit specification, and resolve the regime before computing USD. Never
repair units only from cross-sectional price consensus or promote one legacy
rule by convenience.

## Actual public clocks

Bulk filing dates alone do not establish availability. Bind actual acceptance
from retained SEC submissions recent/history members or an authenticated EDGAR
acceptance source, with an independently evidenced timezone convention.
Inventory accession/form/report/filer must match the bulk row. Preserve missing
acceptance/report date as NULL and a keyed exception. Retrospective ZIP retrieval
clock and Last-Modified are source-version provenance, not filing availability.
The legacy filed+46h rule and sampled HTML-derived clocks cannot stand in for
per-accession public evidence. After-hours filings and DST/timezone ambiguities
must retain actual clocks: a late-evening acceptance is not normalized to the
prior close or report date. A filing-date-only bulk row remains clock-missing
until independently matched; current-quarter clock gaps stay in the denominator. Store all original and amendment clocks and use
strict visibility; no late source is backdated to a report date/deadline.

Retrospective expected inventory recovery has zero planned newHTTP requests:
successive <=10000-main retained-archive partitions after the last processed
member, common archive identity/through/date cutoff, authenticated directory
coverage and full count985667. From initial10000, at least98 further full-or-final
partitions are needed under that bound; the CIK19617 oversized exception requires
a separate reviewed streaming decoder with explicit byte/record bounds. Do not
increase the4MiB member bound silently or count a skipped member as recovered.
History references, duplicates and key coverage must reconcile before claiming
the retrospective inventory complete. That completion is never a2020 manager
population certificate.

## Bounded acquisition mechanics and proposed attempt ceiling

No existing foreign downloader/ingestor may be invoked. Its default four retries,
implicit redirects, replacing publication and deletion/quarantine policies are
not the approved new contract. A future clean landing helper must be independently
reviewed and root-scheduled before any request.

- One retained catalog URL, up to64 catalog-confirmed archive URLs, up to4
  catalog-linked official documentary URLs, and up to32 independently approved
  SEC quarterly index URLs: at most101 logical resources. For independent index
  planning, candidate generation is
  `https://www.sec.gov/Archives/edgar/full-index/{year}/QTR{q}/master.idx`
  for2019..2025 QTR1..4 and2026 QTR1..3:31 candidates, not verified live endpoints
  in this task. Retained trustworthy index copies would consume zero requests;
  presence is currently unmeasured. Do not silently substitute one expected
  submissions inventory for independent index reconciliation. Daily/current
  index gaps or supplemental versions require an enumerated scope revision.
- At most3 **total outbound attempts per logical resource**, including redirect
  hops, failed responses, transport failures and retries. Disable automatic
  redirects and transport retries. Count/reserve each outbound attempt in the
  controller global ledger before sending. Manual redirect chains have at most
  one hop, same approved SEC HTTPS origin; redirect plus retry still consumes
  the same3-attempt ceiling. No HEAD/probe/range request outside this ceiling.
- Upper planning bound:101*3=303 attempts. The31-candidate ZIP core plus31
  quarterly indexes, catalog and one documentary resource would be64 resources/
  192 worst-case attempts only if
  catalog/rule inspection confirms that exact scope. These are bounds, not
  allocated requests. Every additional documentary/individual-accession request
  requires a new enumerated scope and budget decision.
- Initial acquisition pilot, if subsequently granted: catalog then one confirmed
  `2020q1` bulk ZIP, its linked documentation and independent2020QTR1 index,
  at most4 resources/12 attempts.
  Full source qualification remains incomplete regardless of pilot success.
- Shared approved SEC user agent/rate limiter, serial acquisition, explicit
  timeouts and controller <=1GiB memory guard. Stop on SEC/limiter block; never
  clear a block, switch host, or bypass throttling automatically.
- Stream as-served bytes with <=256KiB buffers and incremental SHA256; request
  identity content encoding or record any representation transformation explicitly.
  ZIP response cap512MiB; index cap64MiB; page/document cap8MiB. Reject oversized declared bodies
  before reading and enforce streaming caps if length is missing/inaccurate.
  Content-Length mismatch/status/content-type/HTML-instead-of-ZIP are exceptions.
- One immutable generation/attempt directory. Partial files and failure evidence
  use unique names; never append unproved resumed ranges or overwrite an older
  ZIP/receipt. Full bytes are retained after parsing. Atomic refuse-existing
  object publication plus final receipt commit marker; collisions/interruption
  preserve forensic partials. Receipt stores original/final URL, every attempt/
  redirect/status, UTC request/retrieval clocks, SHA/bytes, response headers,
  catalog identity and member directory identity. Failed/absent outcomes remain
  explicit;404 is not proof the filing or economic position never existed.

Budget hold: the shared60000 all-attempt limit is unresolved. Current conservative
baseline30352 and108494 historical earnings-store entries/uninstrumented retries
are different evidence and cannot be subtracted or reconciled by this plan.
The arithmetic60000-30352 is not authorized remaining capacity. No implicit
legacy1200/other allocation is inherited. Acquisition stays held until the
controller reconciles the objective interval, hidden retry/redirect attempts and
all stores, then grants the exact root-owned scope/reserved attempt ceiling.

## Bounded local ZIP/TSV processing and disk

Future reader verification is separate from network acquisition. One archive per
guarded job, DuckDB128MB/one thread, streaming rows; no `read_bytes()` of an archive
or full TSV/member, no unbounded pandas/in-memory ZIP directory. Bound central
directory to8MiB/256 members and reject oversized/duplicate/encrypted/unsupported
compression/path-traversal/reparse entries. Cross-check central/local headers,
member sizes/CRC and authenticated raw archive digest. Stream every member's CRC
under an explicit8GiB/member and16GiB total-uncompressed/archive ceiling; a bound
stop remains incomplete, even if four required members passed. Optional members
are preserved rather than ignored. TSV records have an explicit1MiB byte bound;
unknown columns remain raw, and malformed/oversized rows keep accession/member/
offset diagnostics. Final table/cover/key validation never drops failed rows from
the expected denominator. Split oversized members into bounded reviewed logical
units; do not treat truncation as EOF or silently raise a bound.

Proposed disk ceiling for a generation:32GiB total retained ZIP bytes **including
failed partial attempts**,2GiB retained independent indexes including failed
partials,2GiB maximum working spill/parsed staging,256MiB total catalog/doc/
receipt metadata; stop before exceeding36.25GiB. The31 candidate core
has a15.5GiB successful-ZIP ceiling at512MiB each; observed sizes remain unmeasured.
Each next job reserves its remaining per-object cap+scratch+2GiB free-space margin
before start; every write charges the actual byte ledger. Failed partials are not
silently deleted to create capacity. If source/parsed size exceeds the chosen
limits, report the keyed incomplete unit and seek a revised disk/unit plan.
No full extraction tree is necessary: stream retained members into bounded
staging/DuckDB units, retaining the original ZIP as bronze.

## Gates before any full-source qualification

1. Confirm the frozen official catalog/source-document scope; reconcile every
   filing-window gap/overlap/revision and the September tail.
2. Verify actual retained ZIP bytes/receipt/directory/member CRCs and all reader
   inputs, including optional managers/notices/schema documents. Unsupported
   sources/member bounds remain FAIL/UNMEASURED.
3. Complete independently authenticated expected submissions/history inventory;
   reconcile HR/HR-A/NT/NT-A accession sets with bulk SUBMISSION, including
   missing/unexpected/duplicated accessions and old-quarter amendments. Report
   absolute counts and every full quarter/window denominator.
4. Per accession: original form/filer/report, real public clock, cover count/raw
   total, units, table keys, notices/shared/combination/confidential omissions,
   amended position overlap and retained original requirements must close from
   independent documentary evidence. Otherwise emit qualified-missing diagnostics.
5. Keep bronze-source, contemporaneous cohort, economic ownership and structural
   producer approval separate. R16 baseline/arrived/retrospective states remain
   distinct; absence from a qualified report may establish reported_holding_zero
   only after exclusions/omissions/manager checks, never economic ownership zero.
6. Root-scheduled native build and independent review then bind admitted R15
   universe/R17 fundamentals generations, dated links/shares/splits and honest
   full all-three-universe coverage. Preserve all original Task11 gates; partial
   source recovery or test passes do not satisfy them.

Decision requested of controller after budget/source inspection: whether the
official retained bulk archive can qualify the table-bronze contract, which
documentary/exception originals are additionally mandatory, and the exact
reserved pilot attempt/disk limits. No request or certification is made here.
