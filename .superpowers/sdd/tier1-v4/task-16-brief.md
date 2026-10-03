### Task 16: Golden research panels `golden_panel/{monthly,daily}`

Deliverable: research-ready panels keyed (session_date or month_end, security_id) with cik, permco-like issuer id,
universe flags (Task 2), returns (ret, ret_excess, dlret-adjusted next-period return label kept in a separate
`labels` table), me, Compustat-std as-of items (Task 3), ratios (Task 4), factor exposures (Task 6), JKP
characteristics (Task 7, monthly), earnings/guidance/ownership/short/events/text fields that exist (Tasks 9-15),
each column with source stage and clock in the manifest. Monthly: 2020-01..latest (warmup history allowed in
inputs, not output); daily: 2020-01-02..latest, year files. Gates: unique keys; complete `univ_all_common`
membership alignment; per-column coverage table per year; PIT audit — for 200 random (date, security, column)
cells recompute the value from the source stage with the GC1 rule and match exactly; a reader helper
`load_golden(start, end, columns, universe)` with memory-bounded scans. Independent of the foreign `gold.py`
alpha-admission panel (different stage name; do not edit it). Files: new `alpha_panel/golden_panel.py`, tests,
`docs/datasets/golden_panel.md`. Report: `task-16-report.md`.

Owner extensions: read `task-16-evaluation-addendum.md` and `owner-coverage-addendum.md`. Gold panels must include PIT 13F, profitability and growth families, bronze-to-gold lineage and qualified predictive evidence at 5/21/63 sessions.

R15 release foundation dependency: read task-2-cap-ruling.md and review-T2-r5-prebuild-findings.md. Before golden materialization, implement independent complete reader-input snapshots and typed exact universe-key admission around the existing year/classification engine, with a separate immutable universe_release_v1 candidate generation and authenticated bounded continuation. Do not edit Task2 modules or trust their generic cache certification. Carry every original gate/denominator/French failure and require independent review before a real build. This precursor is not full Task16 completion; golden panels, labels/evaluation, source admission, consumer export and acceptance remain required.

R9 source consistency: before final golden materialization, require quiescent completed producers and verify the actual input file SHA/size set against each recorded manifest. Stable manifest hashes alone are insufficient after an interrupted partition rewrite. Recheck input snapshot integrity before accepting/publishing evidence; fail on changed or inconsistent inputs and rebuild on a complete snapshot. Source-certified segment partitions and unverified tagged structures must remain distinguishable in gold eligibility/quality metadata.

R12 binding implementation dependency: read `task-14-cap-ruling.md` and review-T14-r5-findings.md. Implement fail-closed notes/segment admission, not a documentation-only warning. Flat source_verified_reportable and source_declared_external_revenue flags are insufficient. Initially pin and inspect the nine reviewed curated proof rows; exclude automatic list proofs and applied external selections, unverified geography and ambiguous tax_dtl notes from certified gold. Test proof/period/source-SHA mismatches, future automatic additions, source-context/bridge incompatibility and amendment carry. Certified segment coverage remains low and gates remain FAIL; never change denominators or call this a parser fix.

