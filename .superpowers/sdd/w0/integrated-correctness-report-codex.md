# W0 integrated correctness qualification

Status: PASSED for source `b185d056440704e7ebcfe2b9395601d7e5264269`. W0 remains open for final changed-target closure and the quiet performance gate.

Root pool-2 executed `.superpowers/sdd/w0/run-integrated-gate.ps1`; unified session56041 exited0. Build: isolated equity-dev, PCH ON, one compiler,394 output actions,2375.269s. The eight engine groups plus impl and worker built without errors. Later commits during this run changed documentation only.

| Target | Run | Passed | Skipped | Seconds |
|---|---:|---:|---:|---:|
| atx-engine-alpha-tests | 706 | 706 | 0 | 48.014 |
| atx-engine-factory-tests | 299 | 299 | 0 | 66.431 |
| atx-engine-learn-tests | 193 | 193 | 0 | 72.214 |
| atx-engine-data-tests | 239 | 238 | 1 | 16.018 |
| atx-engine-eval-tests | 251 | 251 | 0 | 127.502 |
| atx-engine-combine-tests | 189 | 189 | 0 | 10.490 |
| atx-engine-risk-tests | 471 | 470 | 1 | 134.958 |
| atx-engine-book-tests | 128 | 128 | 0 | 15.046 |
| atx-impl-tests | 605 | 600 | 5 | 498.078 |

Total: **3074 passed, 7 skipped, 3081 run**, zero failures. All nine native test commands exited0.

The data skip is a missing documentation-only ingestion-reference fixture. Risk skips its explicitly opt-in large nightly dense-oracle battery. Impl skips five external-panel/fundamental-zoo opt-ins or synthetic-panel prerequisites; G0 separately ran the frozen pre-2020 L10 fixture. These skips are disclosed, not promoted as exercised acceptance. The runner explicitly excludes external data fixtures that can open 2024/2026 market data, and clears all real-panel/nightly opt-ins. No post-2019 market data was opened.

Evidence is under `C:/atx-wt/pool-2/build-equity/w0-integrated-gate/`: source-sha/status, runner hash, configure/build logs, each executable hash, nine full test logs, exact data exclusions and `summary.json` with log hashes. `results.txt` has a Windows PowerShell mixed-encoding newline prefix; native exits and per-target logs are authoritative.

Reviewed ASan fixture/wiring and outer replay-disclosure changes were merged only after this job ended. They require their final affected-target closure; this report does not pretend that this earlier run compiled them. Native scoped ASan and focused disclosure checks have their own independently reviewed receipts. Numerical engine/benchmark production remains unchanged by those additions.

Compiler audit: engine-test object actions consumed1367.016s, impl-test objects542.113s, production/deps404.108s and links61.543s. This source set had meaningful rebuilds, but whole-library Git-SHA definitions, mutable PCH ownership and dev/hygiene mode flipping also cause avoidable future churn. Build-tool fixes and cache diagnosis are tracked separately.
