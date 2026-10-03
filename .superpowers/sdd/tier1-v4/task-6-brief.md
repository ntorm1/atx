### Task 6: Factor returns + risk input pack `factors/`, `risk_inputs/`

Spec: gap-analysis §2 item 5 (binding). Universe from Task 2 (`univ_all_common`, NYSE breakpoints), book equity /
op. profitability / investment from Task 3 (or v10 directly if Task 3 is not landed). Daily and monthly factor
returns; per line-day exposures. Files: new `alpha_panel/factors.py`, `alpha_panel/risk_inputs.py`, tests. Report:
`task-6-report.md`.

Read resume-contract.md R5/R6: aggregate factor-return products themselves must end before2023, not only validation. Bound inputs before return aggregation. Compustat input now uses schema v2/cstd-pit-v3 with available_at in physical vintage keys; select latest visible per issuer/period. Task5's later fundamentals_v11 rebuild requires refreshing dependent outputs/evidence before Tasks7/16; record input hashes.

