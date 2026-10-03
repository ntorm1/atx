### Task 17: Product catalog + parity scorecard refresh

Owner extension: read `owner-coverage-addendum.md`; catalog 13F bronze-to-gold completeness and profitability/growth primitive/feature coverage, preserving all explained and unexplained gaps.

Deliverable: `atx-db/docs/TIER1_V4_PRODUCT_CATALOG.md` — one section per dataset family (§1 map rows): stage,
keys, cadence, history span, measured coverage 2020-2025 on `univ_all_common`, PIT rule, Compustat/FactSet
equivalent, known limits; plus a rebuilt `catalog.duckdb` (stagelake) exposing every new stage and as-of macro, and
`lake verify` PASS. Update `PARITY_SCORECARD.md` measured rows for families this plan touched (if clean). Report:
`task-17-report.md`.

R9 integration evidence: verify actual stage output SHA/size sets, not only manifest-to-manifest hashes, with completed/quiescent producers. `lake verify` must detect incomplete or changed partition publications and report the stage as invalid; do not certify a catalog over stale manifests beside replaced files. An interrupted Task2 build demonstrated this state during execution. Preserve final complete input/output snapshots and refresh all provisional GC3 evidence before acceptance. Any broader atomic-publication limitation remains explicit rather than being hidden by a manifest-only check.
