# Buyback announcements and Part II executions

Current public build/replay contract: [native admission](distributions_admission.md).
Legacy catalog parsing and manufactured hash-only parse receipts are refused. CLI requires
an externally expected complete admission hash; parsing and publication re-read original
objects, match original source receipts, pin exact reader/parser code before computation,
check every input afterward, and compare every admitted native-derived output row.

Outputs:`events/buyback.parquet`, retained `events/buyback_products/<id>/buyback.parquet`
and immutable product manifest. Schema:`atx.alpha-panel.events.buyback/v3`. Public exact
base columns/types:`OUT_COLUMNS`; complete derived schema is independently regenerated
by `validate_product`. Key:non-null unique(accession,source_sha256,event_type,evidence).
Native source truth remains unqualified even when byte/schema/replay validation passes.

New fields:source_sha256(string,original-byteSHA),execution_period(string,month/year),
execution_shares(double,shares),execution_average_price(double,USD/share),
public_program_shares(double,shares),remaining_authorization_usd(double,USD). Source
identity/evidence/accession and explicit conservative receipt clock survive every row.
Announcements retain new/increase types; execution never becomes first_announcement.
Authorization first_announcement is currently NULL with explicit
first_announcement_basis=unknown_incomplete_global_history. Executions are false with
basis=not_applicable_execution. A document-hash partition cannot establish complete
prior180day issuer authorization history; even successful publication of one or more
partitions must not classify an isolated later filing as first. Independent global
prior-history admission is not implemented, so producer and public replay preserve
unknown classification until that dependency is completed.
Future XBRL corroboration retains its own future availability and cannot inform the
announcement-time value. Issuer-primary identity joins are not dividend-class certification.

The narrow PartIIItem2reader requires exact unscaled USD/share captions and numeric
month/year rows. Any scaled/ambiguous section or surrounding shares/amounts scale label
is rejected with keyed unsupported diagnostics. Different captions, unsupported units,
totals and public-program shares above total shares remain unsupported. This preserves
unknowns rather than inventing unscaled share/dollar amounts. Actual catalog contains no
10Qrows; this does not establish absence of retained10Qdocuments elsewhere.

Availability uses matching original receipt fetched_at/done_at as a conservative floor;
source acceptance/class semantics remain unqualified. Events have no expiration. Daily
queries require the prior admitted exchange session22UTC; monthly queries require the
admitted month-end session22UTC. Product replay uses128MBDuckDB/1thread under the guard.

Gate:200distinct independently reviewed native events with>=95%agreement. Actual checked
0/200,FAIL. Actual2020?2025coverage and production build remain unmeasured. Root bounded
source feasibility is not event handcheck evidence or gate acceptance.
