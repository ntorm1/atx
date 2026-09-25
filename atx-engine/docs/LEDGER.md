# Alpha engine ledger

Append-only facts for the alpha-engine DAG series. The former atx-vol ledger was removed
with that module; this is the continuation location recorded in W0 progress (P-8).

- 2026-09-25 I-24: the saved L9 guard-run family blend has validation net Sharpe -1.218738142997244 and one-sided p 0.9503335211679915, not +0.6; reused 2019 net Sharpe is 1.729687486711181 with three prior reads. Source C:/atx/data/equity_mine_l9_guard_20260923/gate_report.json, SHA256 cd8ac431e797d902fd93a558184a26d919f809336c95b6f906b1ed7e27b15824. Status document corrected; all 2013-2019 results remain development evidence.
- 2026-09-25 build coordination: scripts/atx-build.ps1 -Jobs controls ctest only; CMAKE_BUILD_PARALLEL_LEVEL controls native build parallelism. Isolate FETCHCONTENT_BASE_DIR per pool/preset and wait for configure completion before starting the build. Lease auto-configure with dev still fails on removed optional modules after publishing a valid lease; use equity-dev/equity-rel/equity-bench.
