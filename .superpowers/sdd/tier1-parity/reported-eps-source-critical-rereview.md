# Reported-quarter EPS source: Critical-only static rereview

**Disposition: reject the current completion artifact.** This review inspected
`reported-quarter-eps-draft/integration-final.patch` and its owned source and
test mirrors against `reported-eps-source-review.md`. It did not apply either
patch, import code, run tests, access a database or network, or change live
source. Line numbers below refer to the draft mirrors unless marked as patch
lines.

## Remaining Critical

1. **The Chevron-shaped quarterly diluted row still cannot be extracted.**
   `_label_lineage` clears all inherited labels when the preceding row has a
   numeric cell (`src/atx_db/press_release.py:516-519`). The new structural
   fixture has a `Net income ... per share:` parent, then numeric `- Basic`,
   then numeric `- Diluted` (`tests/test_reported_quarter_eps_source.py:110-115`).
   At the diluted row, clearing on Basic leaves only `- Diluted`; the required
   `per share` check at `press_release.py:605-608` fails. The fixture's asserted
   1.39 at test lines 119-124 therefore contradicts the implementation.
   The original production Chevron shape likewise has a numeric Basic sibling
   before `- Diluted`. Preserve the applicable section/parent label across
   numeric siblings while keeping Basic, adjusted, continuing and annual/YTD
   siblings outside the selected diluted lineage. Statically trace the revised
   row hierarchy through both the fixture and actual EX-99 structure, then run
   the focused parser test once the guarded loader is terminal.

2. **The advertised standalone patch does not install the 0320 migration.**
   `integration-final.patch` has six `diff --git` sections (patch lines 1, 938,
   1045, 1113, 1219, 1234), but no `migrations/registry.py` delta. The owned
   registry mirror registers `bodies_0320` at lines 146 and 290, while the
   patched candidate query requires its `acceptance_datetime_raw` column
   (`src/atx_db/sec_submissions.py:79`) and the loader requires its receipt
   table (`src/atx_db/press_release.py:790-809`). Applying this patch alone to
   the stated baseline leaves migration 0320 unapplied; the source path cannot
   run. Assemble the standalone patch with the registry delta and other claimed
   owned files, then repeat the static apply check against that final artifact.
   Its missing test, source CLI and readiness deltas also make the fix report's
   "complete owned mirrors" claim inaccurate; those omissions should be
   reconciled before integration.

## Original Critical dispositions

- **Chevron table and row hierarchy: open.** The span-expanded grid retains
  `rowspan`/`colspan` (`press_release.py:246-336`) and the header selector binds
  the current-year leaf to its quarterly parent and excludes `Year Ended`
  (`462-498`). The inherited-label reset above still prevents the target
  diluted fact. The added fixture has the relevant duplicate quarter/annual
  2025 leaves and adjusted/basic siblings, but its acceptance assertion is
  currently unsupported.
- **Atomic accepted receipt and fact: statically closed.** Accepted insertion
  and fact insertion share one outer transaction (`press_release.py:994-1006`);
  the writer can avoid nesting (`1473-1502`). The terminal query requires an
  accepted receipt's id and document SHA on a matching fact (`790-811`), and
  excludes `fetch_failed` from terminal outcomes (`793-799`). The injected
  fact-write-failure and resume fixture covers rollback and retry at test lines
  160-199, subject to deferred execution. This disposition does not assert a
  runtime pass.
- **13/14-week boundary qualification: statically closed, with coverage gap.**
  The header must provide 13/14 weeks and the report-date end (`435-459`,
  `462-498`); a document fiscal-quarter label is required (`523-565`,
  `922-929`). The source records the exact inclusive start, week count, duration
  and fiscal evidence (`539-547`, `981-986`) without calendar-quarter mapping.
  A 13-week positive and 14-week missing-fiscal negative fixture exist at test
  lines 128-157. The requested 14-week positive and 53-week issuer fixtures
  are still absent, so the repair report should not claim those are verified.

The cache and bulk metadata additions address the earlier Important findings
in source, but the final patch omits their tests. Focused runtime verification
remains deferred under the active loader guard.

## 2026-09-22 repair rereview — patch SHA-256 `2569830B872138B8DB240000F16A293BD0DC68C68D6BAA0B7A058832602C94E5`

**Disposition: one new Critical remains; the two previously open findings are
statically closed.** This append-only rereview inspected the revised eleven-section
`integration-final.patch`, owned mirrors, and the revised network-free fixture.
`git apply --check --whitespace=error` passed against the current baseline.
No code was imported or executed, no tests ran, and no database, network, or
live source was touched.

1. **Critical — an earlier comparative fiscal label can be attached to the
   current EPS cell.** `_reported_fiscal_quarter` returns the first quarter
   phrase anywhere in the document whose year equals the headed end year or
   the preceding year (`src/atx_db/press_release.py:605-629`). For a table
   explicitly headed `Three Months Ended December 31, 2025` with current EPS
   1.39, text that says `Fourth Quarter 2024` before `Fourth Quarter 2025`
   causes this function to return `(2024, Q4)`. The loader then pairs that
   fiscal identity with the 2025-headed EPS and `period_end=2025-12-31`
   (`press_release.py:994-1001`, `1024-1028`). The new Chevron fixture tests
   only current-first ordering (`tests/test_reported_quarter_eps_source.py:105-106`).
   This is a wrong accepted accounting fact, not merely missed coverage.
   Bind a document's current fiscal-quarter label to the selected quarterly
   header/period using explicit source structure; reject ambiguous labels.
   Preserve valid 52/53-week fiscal-year labels whose headed calendar end is
   in the following year. Add a comparative-first fixture proving that a
   prior-year label cannot relabel the current EPS.

2. **Prior Critical — Chevron structural parser: statically closed.** The
   revised lineage walker skips the numeric indented Basic sibling
   (`press_release.py:563-566`), retains the adjacent `per share` and net-income
   parent labels (`568-575`), and stops at an older per-share section so the
   adjusted diluted sibling remains excluded. The revised fixture separates
   those two parent rows, includes quarterly and annual 2025 leaves plus
   Basic/adjusted siblings, and asserts current 1.39 versus prior 1.84
   (`tests/test_reported_quarter_eps_source.py:102-143`). The acceptance
   assertion is now supported by this static control-flow trace; runtime
   validation remains deferred.

3. **Prior Critical — missing migration registry: statically closed.** The
   final patch now carries `migrations/registry.py` import and list entry for
   `bodies_0320` (patch lines 1591-1609), plus the owned tests, source CLI,
   and readiness delta. The standalone patch has eleven diff sections and
   passes the static apply check. The prior six-section artifact finding is
   superseded by this specific patch hash.

4. **New production event-date Critical: statically closed for the Chevron
   shape.** `_document_quarter_end` combines a quarterly duration group's
   month/day with its own year leaf and selects the latest headed end no later
   than filing (`press_release.py:462-498`). The loader calls it with filing
   or event date only as the cutoff, passes the headed end to extraction, and
   stores the 8-K `reportDate` separately as `event_report_date`
   (`press_release.py:991-1001`, `1027-1043`). The fixture uses event date
   2026-01-30 and headed quarter end 2025-12-31, asserting the distinct fact
   date and provenance (`tests/test_reported_quarter_eps_source.py:219-274`).
   Fiscal-label binding remains subject to finding 1.

The prior atomic receipt/fact and week-boundary closures remain unchanged in
this narrow rereview. The new 14-week/53-week positive and mismatched-end
fixtures are present at test lines 178-216; their assertions have not run.

## 2026-09-22 final narrow rereview — patch SHA-256 `30F1DE0D621CEA54FDFCE50940535C295D1590D63E2E799EFB46F22CAC435AB7`

**Disposition: reject pending one remaining Critical fiscal-identity repair.**
This rereview supersedes the prior patch-hash disposition for the changed
parser and fixture. The eleven-section patch passes
`git apply --check --whitespace=error` against the live baseline. No imports,
runtime tests, database or network access, or live source edits occurred.

- **Comparative-first Chevron fiscal label and wide table: statically closed.**
  For the compact official-markup-shaped fixture, quarterly `2025` spans
  currency/value/spacer columns 3-5, while quarterly `2024` spans 9-11;
  annual siblings occupy separate groups (`tests/test_reported_quarter_eps_source.py:47-67`).
  `_table_header_rows` stops before numeric data (`press_release.py:462-469`),
  `_header_columns` treats adjacent identical header traces as one qualified
  quarterly leaf (`510-549`), and extraction scans that leaf for its one
  numeric cell (`688-735`). Its value is column 4 = 1.39; the separately
  headed prior group yields column 10 = 1.84. The lineage strips currency
  and numeric cells while retaining the applicable per-share and net-income
  parents (`552-593`); adjusted 1.52 is excluded. The comparative-first
  phrase in this fixture yields current `(2025, Q4)` because it is the unique
  fiscal label equal to the headed December 2025 year (`623-655`). These
  are static traces of the code and fixture, not test results.
- **Critical — fiscal year after the headed calendar year is excluded, and a
  comparative can be accepted instead.** `_reported_fiscal_quarter` admits
  only label years equal to `period_end.year` or `period_end.year - 1`
  (`press_release.py:647-648`), then prefers a unique equal-year label for
  ends in March-December (`651-655`). Consider a 52/53-week issuer's explicit
  13-week first quarter of fiscal 2026 ending December 2025. Its release
  labels `first quarter 2026` and compares with `first quarter 2025`; the
  quarterly statement header explicitly ends in December 2025. The current
  fiscal-2026 label is discarded; comparative fiscal-2025 is selected, and
  the source then stores current EPS under fiscal `(2025, Q1)` with a 2025
  quarter end (`press_release.py:1033-1040`, `1063-1066`). Without a
  comparative phrase, the valid current quarter is rejected. The same
  error applies to fiscal years ending early in the next calendar year.
  Bind the current fiscal label to the selected earnings-quarter evidence
  without restricting fiscal years to the headed calendar year or its
  predecessor; reject ambiguous associations. Add a noncalendar 13/14-week
  fixture with fiscal year one greater than the headed end year and a
  comparative-first label, asserting the current fiscal identity and exact
  week boundary. The existing 53-week Q4 fixture (`tests/...:225-245`)
  covers the preceding-year case only.

The atomic receipt/fact, retry, registry, and event-date closures from the
earlier review are unchanged. Runtime proof remains deferred under the
project guard.

## 2026-09-22 fiscal-year-ahead repair rereview — patch SHA-256 `49AE4E22C97D5824F0D105143147087401BCF8A0244B11B9A10D587991D46A7E`

**Disposition: the exact fiscal-year-ahead case is statically closed, but one
Critical fiscal-label ambiguity remains.** This is a narrow inspection of
`_reported_fiscal_quarter` and its new fixture. The eleven-section patch
passes `git apply --check --whitespace=error`. No code was imported or run,
and no tests, database, network, or live source edits occurred.

- The helper no longer gates fiscal labels by headed calendar year or month
  (`src/atx_db/press_release.py:623-661`). It recognizes the comparative
  fiscal 2025 label and current fiscal 2026 label in the new 13-week Q1
  fixture; only the current label is immediately followed by `results`, so
  the helper returns `(2026, Q1)` (`tests/test_reported_quarter_eps_source.py:96-119`).
  With two labels and no such anchor it returns `None` (`press_release.py:657-661`;
  test lines 89-93). The April 27, 2025 headed end and January 27 inclusive
  start follow from the explicit 13-week header. The prior calendar-year
  assumption is removed; the earlier closed parser, registry, event-date,
  atomicity and cache dispositions remain unchanged.
- **Critical — a comparative `results` phrase is mistaken for a current
  headline.** The helper strips all HTML tags before scanning (`press_release.py:633`)
  and marks any fiscal phrase followed immediately by `results` or `earnings`
  anywhere in the document as a results headline (`638-656`). For the same
  April 2025 current-quarter table, prose `First quarter fiscal 2025 results
  compared with first quarter fiscal 2026` makes the comparative `(2025, Q1)`
  the only marked label. Lines 657-658 return it, and the loader can store
  current 2026-Q1 EPS under fiscal 2025-Q1. This is the same wrong-accepted-fact
  failure when comparative prose uses the `results` noun; the fixture only
  covers the inverse wording (`tests/...:98`). Bind a current quarter to an
  identifiable title/heading or equivalent explicit current-result context;
  when the document presents competing labels without that evidence, reject
  it. Add the inverted comparative-first wording as a negative fixture.

The above is a source-label correctness blocker only. Runtime verification
remains deferred under the host memory guard.

## 2026-09-22 structural-title closure — patch SHA-256 `BAC6C8DE5808B5FCF2897E2AB3DADD2F962F1D808762309F07A787F900C0B170`

**Critical-only disposition: closed on static inspection.** The last
comparative-prose blocker is resolved in this patch. `_fiscal_labels` gathers
document labels without calendar-year gating (`src/atx_db/press_release.py:623-646`).
`_reported_fiscal_quarter` now marks labels only inside a unique HTML
`title`/`h1`-`h6` or a centered, bold SEC-style `div` ending in `results` or
`earnings`; it excludes comparative wording inside those titles
(`649-691`). A prose sentence saying `first quarter fiscal 2025 results
compared with first quarter fiscal 2026` supplies two unanchored labels and
returns `None` (`tests/test_reported_quarter_eps_source.py:90-98`). The new
fiscal-2026 Q1 fixture has a current-result `h1` ahead of comparative prose
and preserves its explicitly headed April 27, 2025 13-week quarter and
January 27 start (`tests/...:101-125`). The compact Chevron fixture uses a
centered bold current-result title, retaining Q4 2025 despite comparative
prose (`tests/...:47-85`).

The preceding parser, year-group, event-date, atomic receipt, week-boundary,
and registry closures are unchanged. The eleven-section patch passes
`git apply --check --whitespace=error` against the current baseline. This is
  a static closure only; no imports, tests, database or network work occurred.

## 2026-09-22 SEC filing-index follow-up — patch SHA-256 `5B6332C1AF3EE7E5BA6115F64010B5DECBE89DF8035D707BEFC3430BC4663A23`

**Disposition: the Chevron exhibit-discovery defect is statically closed;
one new Critical retry-outcome defect remains.** This bounded inspection used
the saved official CVX filing-detail HTML and the follow-up patch. No code was
imported or run, no tests, database or network access occurred, and no live
source was edited. The follow-up already appears in the shared live files, so
`git apply --check` against those files reports that its hunks do not apply;
this is not an independent patch-content failure.

- **Original production EX-99 discovery: statically closed.** The saved SEC
  index's `Document Format Files` table has the Chevron exhibit at Type
  `EX-99.1`, with document link
  `.../000009341026000019/a12312025ex9918-k.htm`
  (`cvx-q4-2025-official-filing-index.html:103-124`). The follow-up fetches
  the accession's `-index.html` rather than treating directory JSON's MIME
  `text.gif` as an exhibit Type (`atx-db/src/atx_db/press_release.py:340-346`).
  Its parser reads only Type cell 3 from the filing-detail document table,
  verifies the single document href is a flat HTML/text file in the requested
  accession directory, and returns the official attachment name
  (`press_release.py:407-471`). The network-free fixture checks the real
  filename, misleading MIME/name, and wrong-accession href in the follow-up
  patch's `test_filing_detail_type_is_authoritative_and_href_stays_in_accession`.
- **Critical — an unrecognized index page becomes a permanent rejection.**
  `_SecFilingDocumentTable` records rows only after it sees a table whose
  summary is `Document Format Files` (`press_release.py:418-448`).
  `_ex99_documents` returns `()` both when that valid table has no EX-99 and
  when a 200 response is not a filing-detail page, is truncated, or changes
  structure (`451-471`). The loader then writes `rejected` with
  `ex99_document_not_found` (`1077-1083`), and its terminal predicate skips
  every later retry for that accession (`985-1006`). A transient 200 error
  page or parser-structure miss therefore permanently loses a candidate.
  Track whether a valid filing-detail document table was recognized; treat
  absent/malformed table as `fetch_failed` or another retryable outcome, while
  preserving terminal `ex99_document_not_found` for a valid parsed table with
  no typed EX-99. Add a fake-session fixture showing the malformed/200 first
  response is retried and the next valid index reaches the exhibit.

The source clock and simple header-fixture corrections in this follow-up do
not alter the closed parser, fiscal-label, event-date, or atomicity findings.

## 2026-09-22 malformed-index retry draft — patch SHA-256 `FD9B24F274C7EC56F09E54BAD9D802B3ED8EE718E3C2F6D800FEABD11E0DA93C`

**Disposition: retry distinction repaired in principle, but reject this draft
for a new production Critical.** The patch tracks whether the filing-detail
`Document Format Files` table is recognized and complete; absent/incomplete
tables raise `ValueError` (`sec-index-retry.patch:8-93`). The existing loader
catches that exception and records `fetch_failed`, which remains retryable.
The new malformed-200 and valid-no-EX99 fixtures express the intended
different outcomes (`sec-index-retry.patch:133-190`). This was static review
only: no imports, tests, database or network work.

**Critical — the stricter row validator rejects the actual Chevron filing
index.** Every data row must now have nonempty Seq and Type cells
(`sec-index-retry.patch:73-75`). In the saved official index, the valid
`Complete submission text file` row has five cells and a document link, but
its Seq and Type cells are `&nbsp;` (saved
`cvx-q4-2025-official-filing-index.html:133-138`). HTML parsing normalizes
those cells to empty text. The parser therefore raises
`invalid_document_format_row` on that later row even though it already saw
the correctly typed EX-99.1 at index lines 119-124. The real Chevron index
becomes `fetch_failed` on every attempt, so the proposed repair cannot load
its target exhibit. Permit blank Seq/Type in the complete-submission row (or
in any structurally valid five-cell row that is not selected as EX-99), while
still rejecting missing/incomplete table structure and keeping a recognized
table with no typed EX-99 terminal. Add the blank-Seq/Type row to a fixture
based on the full saved index and assert the EX-99 filename is still selected.

The earlier parser, fiscal-label, event-date and receipt/fact findings are not
reopened by this bounded review.

## 2026-09-22 blank-cell filing-index follow-up — patch SHA-256 `A8501F28A73B0DA13971ACDE83291CB120B3CDCF7CA3F29F2B366B8582FFCB73`

**Critical-only disposition: closed on static inspection.** The tiny follow-up
removes the nonempty Seq and Type requirements while retaining the five-`td`
row shape and single Document-cell href requirement
(`sec-index-retry-followup.patch:5-14`). The saved official Chevron index's
complete-submission row has exactly those blank Seq/Type cells, five cells,
and one document link (`cvx-q4-2025-official-filing-index.html:133-138`), so
it no longer invalidates the recognized filing-detail table. That row's
blank Type does not satisfy the unchanged `EX-99` Type selector; the earlier
typed EX-99.1 exhibit remains the sole selected attachment. The compact
index fixture now includes the official blank-cell row
(`sec-index-retry-followup.patch:19-29`). The preceding malformed/no-table
path still raises into retryable `fetch_failed`, while a complete typed table
with no EX-99 remains terminal `rejected`.

This closes the last source Critical on static review. Root's focused tests
and full saved-index runtime check are separate verification and are not
claimed here. No code was imported or executed in this rereview.
