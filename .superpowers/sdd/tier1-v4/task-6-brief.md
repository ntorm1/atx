### Task 6: Factor returns + risk input pack `factors/`, `risk_inputs/`

Release recovery binding (R14): use final explicit fundamentals_v11, compustat_std_v11 and ratios_v11 inputs, with their exact manifest and output hashes. Do not silently fall back to the preserved old stage defaults. Code and offline fixtures may proceed before final producers; real source diagnostics/builds need the controller's exclusive guarded slot, and final coverage/correlation evidence waits for completed Task2/Task5 snapshots under R9. Missing historical membership or required source periods remain measured exclusions/failures; latest snapshots must not be backfilled into the 2013-2022 benchmark window.

Spec: gap-analysis §2 item 5 (binding). Universe from Task 2 (`univ_all_common`, NYSE breakpoints), book equity /
op. profitability / investment from Task 3 (or v10 directly if Task 3 is not landed). Daily and monthly factor
returns; per line-day exposures. Files: new `alpha_panel/factors.py`, `alpha_panel/risk_inputs.py`, tests. Report:
`task-6-report.md`.

Read resume-contract.md R5/R6: aggregate factor-return products themselves must end before2023, not only validation. Bound inputs before return aggregation. Compustat input now uses schema v2/cstd-pit-v3 with available_at in physical vintage keys; select latest visible per issuer/period. Task5's later fundamentals_v11 rebuild requires refreshing dependent outputs/evidence before Tasks7/16; record input hashes.

Inherited source discrepancy to diagnose before cap-input certification: Task4 Fix round2 report and t4-r2-comparability.json identify CIK77159/session2022-02-28, same close33.759998 but market_shares15000 versus panel21088000, issuer ME0.5064m versus711.9308m. Neither is presumed correct. Trace source share class, units, issuer-total aggregation and availability independently; do not silently substitute foreign panel values. Report any required clean-source repair to controller for ownership/descendant refresh coordination. Known unresolved economic-basis errors need explicit quality exclusion or uncertainty in factor/exposure eligibility, not unconditional gold certification. This is separate from the French universe-count FAIL.

