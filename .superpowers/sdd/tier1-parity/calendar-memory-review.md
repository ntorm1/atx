# Calendar-map bounded materialization independent review

Reviewed commit `ad6ab7546fcf4cee599ff7890ff2da664972e154` on 2026-09-20 using Codex. Read-only source review against the program's memory ruling, implementation report, original writer diff and new focused test assertions. No tests, warehouse queries, source archive scans, network calls or production edits were performed.

Decision: **clean; no Critical or Important findings.** The change is suitable for monitored production activation under the controller's process-tree memory guard and one-heavy-workload rule.

The full annual-period inference still executes inside DuckDB with the exact original `(source, security_id)` grouping, `arg_max(month, period_end)`, annual/date filter and equality join. Its result is attached to the complete staged input before any batching. A single issuer crossing the 2,048-row boundary therefore keeps the same inference. Equal maximum annual dates imply the same month, so that tie does not change the inferred value.

The unchanged pure oracle receives bounded frames with the original inputs, stable period-ID ordinals and preserved availability/load clocks. Previous batch frames are released before the next fetch; only DuckDB owns the complete input/output staging tables. The final insert still targets the real output table, preserving its defaults and constraints.

Input staging, all batch transforms, output staging, source-scoped replacement and temporary-table removal are one transaction. Empty or entirely skipped input clears only the selected output source. Both later-batch failures and destination constraint failures roll back publication. The six reported passing cases directly cover these contracts, including a 4,100-row single issuer and comparison with the original whole-input oracle; no redundant review rerun was made.

This removes the full-table Python materialization risk in this writer. It does not certify whole-ladder memory usage or eliminate DuckDB spill/runtime costs, so retain the operational memory limits and monitoring already specified by the controller.
