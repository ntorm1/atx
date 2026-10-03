### Task 5: Fundamentals depth `fundamentals_v11` + rebuild of Tasks 3-4

Spec: gap-analysis §2 item 4 (binding). After v11 passes, rebuild `compustat_std/` and `ratios/` on v11 and report
the coverage delta. Files: `alpha_panel/{fund_items,fund_catalog,fundamentals,fund_validate}.py` only if clean
(check GC5 list first), tests. Report: `task-5-report.md`.

Owner extension: read `owner-coverage-addendum.md`; explicitly map and measure all primitives needed for profitability/growth gold families. Diagnose the source/history balance cases in Task 3 final fix evidence without balance-equation plugs.

Interface R6/R7: compustat_std schema v2/cstd-pit-v3 has available_at in vintage keys. Rebuild it using `compustat_std --fund-stage fundamentals_v11`, then rebuild ratios with `--std-stage <published_compustat_stage>`. Ratios consumes standardized fundq/funda, never raw fundamentals_v11; no raw-stage alias is required. Read the Task4 ratio-to-primitives mapping and final report before primitive recovery. Final rebuilt hashes invalidate earlier dependent factors/coverage until refreshed.

Task4 matrix: `atx-db/docs/datasets/ratios.requirements.json` (final reviewed version) identifies exact missing primitives, source/history mismatches and producer dependencies. Preserve R8 WFRS naming: valid issuer-cap P/E extensions are separate from unavailable certified diluted-EPS counterparts. Resolve class/split/date alignment with existing clean market evidence where possible; record any remaining interface dependency explicitly. Task13 owns indicated-dividend source recovery. After source recovery, rerun affected ratio handchecks/gates, not coverage alone.

Task4 review dependency: standardized funda currently lacks source period-start metadata. Adjacent fiscal years, equal fiscal-year-end month and a 350-380-day end-date interval screen out some transitions but do not certify comparable full-year source durations. Publish source-backed period start/end or a verified full-period flag with its availability clock, then consume that evidence in annual ratio growth/average-history eligibility. Retain ordinary missing-history rows in coverage denominators and give invalid/transition periods explicit null reasons. Also resolve the prior-year TXDITC source/history discrepancy identified in Task4's final handcheck report; do not plug the missing prior balance with the current value.

