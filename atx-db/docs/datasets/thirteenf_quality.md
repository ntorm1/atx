# 13F accession diagnostics

Stage `thirteenf_quality`, schema `atx.alpha-panel.thirteenf-quality/v1`.
Source contract and adapter commands: [thirteenf_source.md](thirteenf_source.md).

`accessions.parquet` key accession (VARCHAR normalized18digits). Filing/report
quarters VARCHAR; gate_status VARCHAR; exceptions VARCHAR[]; available_at
TIMESTAMPTZ audit creation clock. `unexpected_parsed.parquet` preserves parsed
keys outside the supplied authoritative inventory with reason and audit clock.
No expiry is inferred: rerun against pinned inventories for each release.

Publication remains UNQUALIFIED even when normalized evidence claims pass.
Source-original verification, complete public-inventory scope, quarter/year
reconciliation and independent source/economic review remain required.
Real-source gate counts and all-three-universe coverage are UNMEASURED.

`bridge-originals --evidence manifest.jsonl --root ABSOLUTE_ROOT --output NEW.jsonl --max-records 1000` creates immutable original-byte diagnostics and a digest receipt. Evidence rows contain expected inventory record and retained receipt; paths must be absolute, inside explicit roots, without reparse traversal. <=10000 rows, <=8MiB object, <=32MiB input manifest, <=256KiB evidence record; compressed objects require a separate authenticated member adapter and are not silently decompressed.

Actual complete SEC submission bytes are compared with receipt digest and SEC accession URL/retrieval clock; original form/accession headers and XML cover entry/value totals reconcile with actual positions. XML declarations of external entities/DOCTYPE are unsupported. Source SH versus PRN and option labels remain explicit. Unknown header acceptance timezone, documentary dollar-unit provenance, shared-manager/confidentiality/omission/amendment chains stay accession-keyed exceptions. Current bridge always sets source_qualified/qualified_original/table_absence_qualified false and public available_at NULL: normalized booleans cannot certify these open gates. Header clock and inventory acceptance remain separate diagnostics. This bridge adds raw evidence parsing, not complete source admission or native ownership assembly.
