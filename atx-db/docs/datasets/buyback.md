# Buyback announcements and Part II executions

`events/buyback.parquet` retains the existing 8-K/EX-99 authorization extraction and
adds eligible retained10-Qdocuments. Explicit authorizations retain `new`/`increase`;
PartIIItem2table executions have `event_type=execution`, never `first_announcement`.
The narrow execution reader accepts a native pipe table with these exact columns:
`Period | Total shares purchased | Average price paid per share (USD) | Shares purchased
under publicly announced plans | Remaining authorization (USD)`. It accepts month/year
rows with unscaled nonnegative numbers, rejects unsupported units/currencies, totals,
and public-program counts above total shares. This does not yet cover arbitrary SEC HTML
tables or differently captioned issuers; actual coverage is unmeasured.

New fields are `source_sha256`(string,native bytes digest), `execution_period`(string,
month/year), `execution_shares`(double,shares), `execution_average_price`(double,USD/share),
`public_program_shares`(double,shares), `remaining_authorization_usd`(double,USD). Every
row preserves source document, accession, evidence and SEC-source availability metadata.
All event fields use the filing acceptance UTC clock; missing source clocks require
upstream conservative publication floors. Daily access is strictly before prior-session
22UTC, monthly access strictly before monthend22UTC. No staleness applies to historical
events. Existing issuer-primary linkage is issuer-level and does not certify a dividend
class. Existing later-XBRL corroboration is explicitly clocked and cannot be used as
announcement-time information.

Parser rows/catalog, fundamental events, fundamental manifest, identity linkage and
identity manifest are content pinned; publication rechecks them before exposing the
current alias. Each immutable product is under `events/buyback_products/<id>/` and
retains its own manifest. Existing products reject overwrite.

Replay only after root source review, under a0.3GiBguard: `.venv/Scripts/python.exe
-m atx_db.alpha_panel.events_buyback --phase all`. Uses128MBDuckDB and1thread. This
command does not fetch documents. Catalog10-Qcoverage may be absent and must be audited
before declaring source completeness. Gate:200distinct native-event checks≥95%; actual
checks0, gateFAIL. No actual2020–2025coverage or publication is measured; fixture tests
do not close that gate. Native source sample/terms/history/budget review is outstanding.
