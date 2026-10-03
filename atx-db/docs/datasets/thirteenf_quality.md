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
