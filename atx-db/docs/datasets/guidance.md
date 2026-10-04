# Native guidance: events_guidance_v2

Frozen semantic SHA256: `94358ee94edc83b699bf00cc64216d61eac1a63b89cdf92d39cfa96adca26a33`.

This isolated publication implements semantic `events.guidance/v2`. It never rewrites
legacy `events/guidance.parquet`, legacy scratch, Task9 outputs or shared registries.
Task17 must resolve explicit phase-qualified products from the validated immutable
manifest, rather than glob generations or read a root parquet path.

Original 8-K 2.02/7.01 releases from2019 and their 8-K/A issue vintages are retained.
The fixed precision denominator selects original releases2020-2025, independent of
extraction success; missing sources and rejected candidates remain in releases/sample.
No HTTP/source acquisition is provided. Unknown periods, currencies, clocks and
incompatible accounting/share definitions remain NULL with reasons.

## Economics and PIT

`guid_surprise` is EPS only, predeclared sign+1 before actual values or return results.
Revenue guidance and native-unit revisions remain present; its surprise is NULL with
`unit_mismatch_total_revenue`. No market-value-normalized revenue formula is introduced.
Latest strictly earlier exact-target/basis guidance is required. An ambiguous or
unqualified latest known comparable issue blocks fallback to an older valid midpoint.
Same-clock and future guidance cannot enter. Text-prior numbers do not substitute for
an independently clocked earlier release. One-sided source bounds are retained but
cannot supply a midpoint/revision/surprise.

Price is the last completed pinned exchange-session close strictly before original
actual issue, with genuine availability before issue and independent positive
currency/class/split/share proof. No later reaction close or older-price fallback.
Daily consumers use available_at strictly before22:00 UTC of the preceding admitted
exchange session. Missing public clocks are never coalesced into visible features.
These are immutable event rows; there is no daily forward-fill or arbitrary TTL.
Exact target/calendar identity provides the comparability boundary; downstream
holding windows must be separately predeclared. No return evaluation or forward labels
are generated; all such complete windows remain strictly before2023 under R21.

## Published columns

Each field below uses original source issue clocks or strict derived event-time
comparability. Strings/identities are immutable provenance; numeric staleness is
exact-target/event-specific rather than a fabricated daily freshness claim.

| Column | Arrow type | Unit | Definition/source |
|---|---|---|---|
| cik | int64 | issuer | SEC issuer identity; pinned filings |
| form | string | text | Original SEC form; amendments retain their own issue |
| is_amendment | bool | boolean | SEC source flag, never replaces original |
| filing_date | date32[day] | date | Original SEC filing date; fixed yearly-report axis |
| period_end | date32[day] | date | Exact independently source-qualified fiscal target; never estimated |
| fiscal_year | int32 | year label | Original source fiscal-year label |
| fiscal_quarter | int32 | 1..4 or NULL | Exact quarter, NULL for FY guidance |
| available_at | timestamp[us, tz=UTC] | UTC | Original qualified SEC acceptance; unknown remains NULL and invisible |
| source_available_at | timestamp[us, tz=UTC] | UTC | Unmodified source issue clock, not an invented public clock |
| candidate_id | string | SHA256 | Stable original document/candidate or actual-basis proof identity |
| accession | string | identity | Original filing vintage |
| source_doc | string | text | Original exhibit identity; actuals retain Task9 document names |
| doc_sha256 | string | SHA256 | Original raw source byte identity |
| measure | string | EPS/REVENUE | Source monetary metric |
| basis | string | GAAP/ADJUSTED | Explicit source accounting basis |
| accounting_basis_id | string | identity | Independently identified accounting definition and operations scope; enum alone is insufficient |
| period_type | string | Q/FY | Original target duration identity |
| fiscal_calendar_id | string | identity | Historical source fiscal-calendar identity; no latest-profile inference |
| currency | string | USD | Original source declaration; unsupported currencies missing |
| unit | string | USD_PER_SHARE/USD | EPS per-share or consolidated total revenue |
| share_basis | string | basic/diluted/consolidated | Original EPS denominator or revenue scope |
| share_basis_id | string | identity | Independently proved original split/class/share bridge |
| value_type | string | range/point/floor/ceiling | Source quote type |
| evidence | string | text | Original candidate clause, not independent precision certification |
| period_basis | string | provenance | Original parser origin label (stated/table/heading/inferred) |
| period_resolution | string | provenance | Stated versus inferred target retained from source resolver |
| period_origin | string | provenance | Sentence/line-prefix/table/heading source origin, separate from explicitness |
| comparator_identities | list of keyed structs | provenance only | Separately retained affected target/basis identities; unknown scope stays unknown; never qualified value mapping |
| qualification_reason | string | text | Exact source/period/basis/clock missingness reason |
| low | double | measure unit | Source lower endpoint/bound only; point quotes do not manufacture endpoints |
| high | double | measure unit | Source upper endpoint/bound only |
| point | double | measure unit | Source point quote only; range midpoint is a separate field |
| midpoint | double | measure unit | Complete low/high mean or source point; one-sided bounds have no midpoint |
| event_kind | string | guidance/actual | Issue-time revision or actual-time surprise event |
| guid_revision | double | measure unit | Current midpoint minus latest strictly earlier comparable guidance midpoint |
| guid_surprise | double | dimensionless EPS | Comparable GAAP diluted EPS actual minus prior guidance midpoint, divided by qualified same-basis price (R22) |
| actual_value | double | measure unit | Qualified Task9 original announcement actual; missing bridge stays NULL |
| prior_accession | string | identity | Chosen strictly earlier comparable guidance filing |
| feature_reason | string | text | Unsupported/ambiguous comparator, price, units or history reason |
| security_id | int64 | identity | Independently proved actual/price security bridge |
| price_session | date32[day] | exchange session date | Last completed admitted close strictly before actual issue |
| price_close | double | USD/share | Positive comparable original close, no reaction price |
| price_available_at | timestamp[us, tz=UTC] | UTC | Genuine denominator availability, strictly before actual issue |

Keys: guidance `(cik,accession,candidate_id)`; features add `event_kind`.
Release audit/sample keys are `(cik,accession)`. Their columns preserve original
form/amendment/filing/public clocks, document SHA inventory, document/candidate/qualified
counts, missingness reason and the frozen SHA sampling key.

## Native source contract

`python -m atx_db.alpha_panel.guidance --native-phase contract` prints required source
columns and all publication schemas without reading data. `guidance.config.json` freezes
formulas/limits/axes and `guidance.registry.json` supplies the integration stub.

An explicit `guidance_bindings_v2` JSON must supply exact `sources`: sec_filings,
ea_actuals, catalog, targets, actual_basis, prices, calendar, members, evidence_contract.
SEC and Task9 producer pins use `{stage,sha256}`. Task9 must be the current native
immutable parsed releases generation, with exact products, schema/parser/code identity
and matching SEC/catalog lineage; ea_publication resolves its product. No legacy or
feature-phase actuals fallback. Other inputs use `{path,sha256}`; evidence_contract
also requires separately expected `reviewed_sha256`. No fabricated production pins
are supplied by this implementation.

The reviewed `atx.guidance-independent-evidence/v1` contract pins all five exact
qualification/population tables, supported rules printed in guidance_sources.bind,
and independent full member counts for every year2020-2025. It is external reviewed
source evidence, not an arbitrary qualified boolean. Each target/actual/price/calendar
proof additionally has evidence_id, original source_sha256 and genuine source_available_at.
Target proof binds the candidate original document and its exact historical target,
currency, accounting definition and share basis. Actual proof must match Task9 original
document SHA, fiscal labels/calendar/end, GAAP EPSFI diluted or consolidated saleq,
and independently recorded source_value exactly equal to the Task9 actual value.
Native EPS split/share qualification cannot be overridden by metadata; missing
quarter labels and false eps_split_comparable remain unsupported.
Adjusted/basic/annual actual bridges are currently unsupported and explicit NULL.
Price proof binds security, exchange close, currency and original share/split identity.
FPI sources/bridges outside native Task9 8-K actuals remain unsupported coverage gaps;
all member issuers still stay in the independently pinned yearly denominator.

Raw objects are cache-only SHA-addressed landed_v2/sec_events objects, streamed with
a decompressed8MiB limit before allocation. Missing, corrupt, unsupported and oversized
objects are recorded, never fetched/quarantined/overwritten. Parser candidate limit256
per document,64documents/release,10000rows/issuer,512-row metadata pages. A breached
history/document-count bound refuses rather than shortening original history/population.
Guarded real resource evidence remains pending; these limits are engineering bounds.

## CLI, restart and validation

All actual data commands require the controller sole source slot and <=1GiB guard.
Use one thread; native DuckDB128MB with file-backed spill. Example authorized commands:
```powershell
python -m atx_db.alpha_panel.guidance --native-spec BINDINGS.json --destination events_guidance_v2 --native-phase parse --max-new-releases 100
python -m atx_db.alpha_panel.guidance --native-spec BINDINGS.json --destination events_guidance_v2 --native-phase features
python -m atx_db.alpha_panel.guidance --native-spec BINDINGS.json --destination events_guidance_v2 --native-phase validate
```

Bounded parse returns partial_cache_only without publication. A content-keyed unit
receipt is committed last and authenticates original filing/code/config/source identity,
candidate parquet SHA/size/schema/row count and release audit. Same-pin resume reuses
only verified units; new inputs or code create a separate cache key. OS publisher locks
release after interruption. Pending generations never activate. Complete generations
are immutable and activation is atomic, with source pins rechecked immediately before
publication. Old generations and failed/private work remain retained.

Public validation verifies manifest/code/config/products/bytes/schema/rows, every
original release against source inventory/authenticated units, fixed sample identity,
exact full feature recomputation and independent yearly coverage. It cannot relabel
byte identity as source/economic certification. No proof-less numeric feature is valid.

## Precision and coverage evidence

Precision >=90% on exactly300 independently source-handchecked original releases.
Selection is the lowest SHA256(cik/accession/task10-precision-300-v1), without parser
success filtering. Manual audit is separately pinned/reviewed, pins immutable parse
manifest/config, covers all selected identities/source document hashes and supplies
field-by-field expected guidance plus reviewer/original-text evidence. A boolean
correct=true alone is rejected. Missing/subset/duplicate/extra judgments refuse; fewer
than300 selected releases cannot pass and the denominator never shrinks.

Actual handchecks:0/300; precision FAIL/unmeasured. Synthetic test judgments are
engineering mechanisms only and are never an actual source handcheck/PASS.

| Year | Population | Guiding issuers / denominator | Share |
|---|---|---|---|
| 2020 | member issuers | unmeasured / full original required | FAIL/unmeasured |
| 2021 | member issuers | unmeasured / full original required | FAIL/unmeasured |
| 2022 | member issuers | unmeasured / full original required | FAIL/unmeasured |
| 2023 | member issuers | unmeasured / full original required | FAIL/unmeasured |
| 2024 | member issuers | unmeasured / full original required | FAIL/unmeasured |
| 2025 | member issuers | unmeasured / full original required | FAIL/unmeasured |

There is no invented coverage acceptance threshold; the required yearly share is
reported on all independently counted member issuers, with missing years explicit.
Actual stage rows/bytes/span/peak memory and all source/economic measurements remain
pending. source_quality_certified=false and NOT_PRODUCTION_READY remain binding.

## Whole-task repair1 comparator and population admission

Table and heading origin is separate from target resolution. Explicit source
labels remain eligible only with the original unique independent proof; inferred
years/targets remain refused. Parser candidate version is now
guidance-rules-v2-period-provenance; the new config/code identity refuses stale
units/generations. No formula, direction, threshold or denominator changed.

Comparator identities are retained separately before value admission. Duplicate
mappings retain common proved identity; conflicting mappings retain their explicit
union. A NULL scope attribute denotes uncertainty only within the original
source-stated target, never a qualified identity. A latest unusable affected issue
blocks older numeric comparators, even when its ambiguous accounting/calendar/
share fields cannot populate a single display identity. Explicitly different
metric/fiscal targets remain unaffected. Proof clocks later than issue cannot
supply a mapping; only the original source-stated core remains as an uncertain
blocker. Mapping count>256 refuses without truncating the possible identities.

A missing-source release with genuinely unknown exact target remains in full
release/sample/coverage failures and creates no invented comparator identity. It
cannot assert a supersession of a known exact target. This policy supplies no
completeness/economic certification; actual missing-source evidence remains FAIL.

Public feature replay refuses NULL/orphan issuer IDs outside the parsed-guidance
plus admitted-actual source union. Global published row count must equal the sum
of every regenerated issuer's full multiset, in addition to per-issuer equality.
Consistent byte/schema/row records alone do not certify the feature population.
