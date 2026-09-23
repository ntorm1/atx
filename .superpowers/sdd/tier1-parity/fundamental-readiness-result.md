# FQ3 implementation result

Implemented the optional FQ1/FQ2 inventory in `atx-db/scripts/measure_tier1_readiness.py`, isolated fixture tests in `atx-db/tests/test_fundamental_signal_readiness.py`, and operator notes in `atx-db/docs/FUNDAMENTAL_SIGNAL_READINESS.md`.

The JSON key defaults to `not_requested`. With `--include-fundamental-signals`, the read-only measurement reports separate FQ1/FQ2 schema gaps, bounded manifest inventories, one snapshot-eligible complete FQ1 run and grouped coverage, and one latest complete FQ2 run with frozen summaries only when its linked complete build has an equal recorded hash. Unknown blocker text and arbitrary manifest JSON are excluded. Certification remains `unmeasured`.

Independent static review found two Important linkage cases. Both are fixed: FQ2 now requires the build snapshot to precede the evaluation snapshot, and a missing FQ1 manifest schema leaves the selected FQ2 run visible with linkage unmeasured. Recorded hash equality is distinct from build snapshot eligibility. Tiny fixtures cover both cases, blocked runs, bounded run and coverage inventories, and candidate suppression.

The final guarded focused test batch passed: five tests (`fq3-tests3-memory.json`, peak 0.570 GiB under a 1.5 GiB process-tree cap). Sequential scoped Ruff passed (`fq3-ruff2-memory.json`, peak 0.062 GiB). The readiness script retains its original CRLF line endings (`i/crlf w/crlf`), and `git -c core.whitespace=cr-at-eol diff --check` passed. The first two guarded test attempts found fixture-only setup errors, then were corrected; Ruff's first pass found one extra blank line in the test imports, also corrected. No live warehouse, migration, network request, commit, or production claim was made.
