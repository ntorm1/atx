### Task 9: Announcement-clock earnings (calendar v2, EX-99 actuals, SUE family)

Spec: gap-analysis §2 item 7 (binding). Files: `alpha_panel/{earnings_calendar_v2,events_sources}.py` (if clean),
new `alpha_panel/ea_actuals.py`, tests. Report: `task-9-report.md`.

Resume from the report's orientation checkpoint; no prior Task9 build/fetch was completed. Read resume-contract.md and the final Task14 report for source-budget accounting. Before acquisition reconcile actual all-attempt counters (including retries/redirects) against the shared 60,000 SEC ceiling; cached source receipts take priority and a lane allocation never increases the global cap. Every fetch/parse/diagnostic/build is guarded at <=1 GiB. Keep event-writer tasks 9/10/12/13 sequential.

Task3 dependency (T3-b): remeasure rdq 8-K 2.02 share using acceptance on or before the same ET calendar date as the standardized row's filing, with the strict timestamp share alongside. Preserve compustat_std v2/cstd-pit-v3 available_at vintage keys and latest-visible selection. Task5's later v11 rebuild invalidates this evidence, so record the exact source manifest/output integrity and hand off a repeatable refresh command. Report the measured gate back to Task3/17; do not silently close it from a changed denominator.

Event-derived features must have clocks reflecting all required inputs. In particular, an announcement-window return cannot be visible at the original release acceptance before its price window has finished. R5/GC2 still prohibit aggregate/evaluation return statistics on or after 2023. No foreign event/gold pipeline is authorized by this task.

R13 conservative reaction-return boundary: compute the new ea_ret_3d only when its entire announcement/reaction window ends before2023; leave later reaction values uncomputed/NULL with the holdout reason. Bound source dates before calculation. Earnings values, clocks, SUE and value/coverage audits remain authorized through latest. This does not change the separate Task7 contract for historical predictor inputs.

