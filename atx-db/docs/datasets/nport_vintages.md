# Actual filing event fund flow vintages

CLI: `python -m atx_db.alpha_panel.nport flow-vintages --filings <part.parquet>
--input-manifest <manifest.json>`; repeat filings for each explicit consumed part.
Native128MBsinglethread,file-backed DuckDB; every actual part SHA/byte identity and
parent manifest are verified before/after. Parent status must be complete.
The stage remains UNQUALIFIED_SOURCE_COMPLETENESS and inherits no certification
for undeclared parent side products. No current-data publication accepted yet.

Keys: fund_key (series_id),report_date,available_at. Every actual filing event
generates new visible report snapshots. Originals persist before amendment clocks;
late amendments are never globally discarded by a filing deadline. A late prior
report amendment changes later report flow denominators only from that public
clock onward. Missing series_id is rejected, rather than collapsed across funds
under a CIK. Missing report dates/public clocks are explicit rejected accessions;
diagnostics carry audit clock and preserve NULL source clock separately.

USD net_flow_reported requires every subscription/reinvestment/redemption
component. Fraction flow_pct_reported additionally requires positive prior net
assets75–105days earlier. Ret_q and return-derived flow_proxy obey the complete
pre2023 window boundary. Available_at is actual filing event, with source
filing_available_at/previous_available_at/accession/previous_accession retained.
Quality_status stays UNQUALIFIED_SOURCE_COMPLETENESS. No unsupported zeros.

Ownership security aggregation, source originals/inventories, reporting-cohort
closure, splits, coverage and economic admission remain separate unfinished gates.
