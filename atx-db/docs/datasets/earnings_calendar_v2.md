# Source-backed earnings calendar v2

New stage `earnings_calendar_v2`, schema `atx.alpha-panel.earnings-calendar/v2`.
Existing `earnings_calendar` is preserved for consumers and rollback.

New v2 publications use immutable `generations/<id>/` directories, activated
atomically by manifest.json. Resolve `products['announcements.parquet']` through
ea_publication.file. Old generations remain available for explicit rollback.
The FPI scratch parser's complete receipt pins classifier, module hashes, catalog
hash, output size/hash and document evidence. Calendar build checks that identity
before admitting FPI rows and again before activation; stale scratch is rejected.

`announcements.parquet` is keyed by `(cik, accession)`. Source categories are
`8k_202`, `6k_results` and `periodic_report`. The first uses 8-K item 2.02 metadata
but gets fiscal period only from its release's own explicit period, via the
stable `ea_actuals/releases_manifest.json`. A later periodic filing never labels
that release at an earlier clock. Unknown release periods stay NULL.

The FPI classifier admits a results release's explicit end date. Fiscal labels
requiring an unqualified historical fiscal calendar remain missing, with
`label_fiscal_calendar_unqualified`. The latest issuer fiscal-year profile is
not a historical clock-qualified calendar. Periodic report fallbacks use their
own report dates and acceptance clocks and remain distinguishable from releases.

Columns inherited from v1 retain their types: CIK/accession/form, EDGAR filing
and acceptance metadata, UTC/ET announcement clocks, fiscal period, timing,
session/reaction session, primary/duplicate flags and expected dates. New columns:

| Column | Type | Meaning |
|---|---|---|
| source | VARCHAR | 8k_202, 6k_results, periodic_report |
| period_basis | VARCHAR | Source period evidence or reason it cannot be qualified |
| is_fpi | BOOLEAN | Issuer with a 6-K/20-F/40-F in the SEC corpus; coverage metadata |
| headline | VARCHAR | Retained FPI source text context |
| clock_reason | VARCHAR | Resolved clock, or unknown/ambiguous acceptance |

`available_at` is resolved EDGAR acceptance, UTC. Unknown/ambiguous clocks are
NULL and cannot supply timing, expected dates or primary flags. Daily visibility
uses the strict previous-session 22:00 UTC rule. Values are announcement events;
there are no labels or returns in this calendar.

Coverage gate is >=95% of member issuers per year from 2020, FPIs included.
Fallback coverage and true-release coverage are reported separately. Final GC3
member_equity/univ_all_common/univ_r3000p measurement is performed by
`ea_actuals --phase audit` after final Task2/Task5 publications and full source
output hash verification. No coverage gate has been measured for this new stage
at the implementation checkpoint.

Build commands, after `ea_actuals --phase parse`, under the shared memory guard:
`earnings_calendar_v2 --phase parse`, then `earnings_calendar_v2 --phase build`.
The calendar pins the stable parse manifest so subsequent actuals feature/audit
publication does not change the calendar's source dependency.
