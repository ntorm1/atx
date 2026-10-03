### Task 17: Product catalog + parity scorecard refresh

Owner extension: read `owner-coverage-addendum.md`; catalog 13F bronze-to-gold completeness and profitability/growth primitive/feature coverage, preserving all explained and unexplained gaps.

Deliverable: `atx-db/docs/TIER1_V4_PRODUCT_CATALOG.md` — one section per dataset family (§1 map rows): stage,
keys, cadence, history span, measured coverage 2020-2025 on `univ_all_common`, PIT rule, Compustat/FactSet
equivalent, known limits; plus a rebuilt `catalog.duckdb` (stagelake) exposing every new stage and as-of macro, and
`lake verify` PASS. Update `PARITY_SCORECARD.md` measured rows for families this plan touched (if clean). Report:
`task-17-report.md`.
