M&A terminal events v2 (engineering preview)
===========================================

Status: native document replay and offline qualification core implemented;
financial native reader/admission and actual immutable stage publication remain
open. No certified v2 Parquet/manifest has been published. The legacy v1 stage
and foreign label modules are preserved.

`mna_native_v2.py` consumes original bytes, not parsed rows. An independently
reviewed immutable caller contract must pin the complete object inventory,
source SHA256, accession/CIK, expected code SHA256 for all three v2 modules,
UTC retrieval and availability clocks. The public validator verifies the full
schema, unique keys and every output value by native replay. Native SEC full
submission identity and acceptance headers are matched exactly; a submission
with multiple documents is unsupported pending a pinned selection contract.
HTML-only originals get retrieval floors unless joined to an independently
pinned original SEC submissions recent entry with exact native issuer/accession
and matching native SGML acceptance. An ISO offset or Z alone does not establish
the SEC bulk field's timezone semantics. Catalog assertions never supply clocks.
Archived arrays without native issuer identity are unsupported. These checks establish document semantics only, not
financial completion, party mapping or terminal returns.

Deal vintage keys: `(accession, source_id, available_at)`; rows carry their own
native document clock. Later terms or statuses never overwrite earlier rows.
The parser retains unparseable/ambiguous candidates; it never treats Item 2.01,
Form 25, issuer primary-line metadata or a caller boolean as financial proof.

Terminal keys: `(security_id, last_session, available_at)`; UTC clock is the
maximum of event, terms, native completion, target and acquirer price clocks.
Daily PIT cutoff requires the previous independently admitted exchange session
at 22:00 UTC, strictly less; no calendar-day substitution. Monthly consumers
use their stamped session's 22:00 UTC cutoff and prices no later than that close.
Staleness: event vintage, no arbitrary carryforward of status or terms.

| Column | Type/unit | Definition/source |
|---|---|---|
| security_id | integer | Independently qualified target line, not issuer primary line |
| last_session | date | Last target close session in complete candidate inventory |
| available_at | UTC timestamp | Latest admitted input availability |
| deal_id | text nullable | Original-source deal identity |
| proceeds_per_share | decimal, currency/target share nullable | Cash or acquirer terminal close times conversion ratio plus cash; conditional qualification only |
| currency | text nullable | Explicit native currency; dollar symbol alone is ambiguous |
| dlret | decimal nullable | Qualified proceeds / target last close minus one, strictly pre2023 only |
| dlret_exact | boolean | False until native financial and price replay admission implemented |
| dlret_imputed | boolean | False; no invented zero or imputed exact return |
| reason | string array | Every unresolved provenance/class/unit/clock/value condition |
| source_ids | string array | Retained original source identities |
| settlement_date | date nullable | Native dated settlement, not filing/item date |

Cash/stock/mixed terms need explicit share class, units, currency and signed
price basis. Adjustment factors and FX must be independently native-qualified;
this core never guesses them. CVRs, elections, proration or unresolved fractional
treatment remain NULL. A terminal return does not certify residual-month cash,
conversion treatment, intermediate prices/actions or a complete forward label.
Every return observation and complete window ends before 2023-01-01; source
and value coverage continue all2020+. The standalone formula is offline-fixture
only, not a builder certification API.

Root metadata reconciliation preserves original2267 raw M&A candidates:
1669 legacy continued=false plus598 continued=true. Legacy continuation is
unqualified and cannot remove candidates. Source-proven terminal/conversion
qualification is still needed for those598; all missing/failed cases remain in
the numeric gate denominator. No gate has been relaxed.

| Actual gate | Status | Measured |
|---|---|---|
| Original candidate reconciliation | PASS (root metadata only) | 2267=1669+598 |
| 200-deal handcheck >=95% | FAIL | Unmeasured; no admitted review corpus |
| Consideration parsed >=80% | FAIL | Unmeasured; no actual native replay run |
| Exact DLRET >=80% of2267 | FAIL | Unmeasured; financial admission/returns not run |
| Per-year/universe2020-2025 coverage | FAIL | Unmeasured |

Known limits: minimal rules support unambiguous literal cash and Parent/Acquirer
stock clauses only; all deal statuses stay unknown absent financial native
completion admission. UTF-8 originals only; other encodings remain unsupported.
No actual source acquisition, HTTP, source/hash scans or data builds were done
by this implementation lane.
