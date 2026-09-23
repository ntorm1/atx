# FQ3: expose recorded fundamental research in readiness measurements

Implement the bounded proposal in fundamental-readiness-audit.md. A desk
operator must be able to distinguish missing research tables, blocked panels,
usable recorded coverage, and evaluated frozen hypotheses in the same
read-only readiness artifact used for the production warehouse.

Own only atx-db/scripts/measure_tier1_readiness.py, a new isolated
atx-db/tests/test_fundamental_signal_readiness.py, and
atx-db/docs/FUNDAMENTAL_SIGNAL_READINESS.md. No migrations, registration,
activation, jobs, builder/evaluator changes or root-owned dictionary edits.
Add --include-fundamental-signals, JSON fundamental_signal_research and a
Markdown section. Default is not_requested; certification stays unmeasured.

Report separately missing FQ1/FQ2 surfaces. Bound run inventories through
Measurement.rows(); retain truncation flags. For details choose the latest
complete run with as_of_date <= report snapshot, deterministic run_at/run_id
ordering. Display selected run ID and snapshot/date range explicitly, so an
older snapshot cannot look current. Observe build/evaluation linkage and
recorded hash equality only; mismatch must not surface candidate results as
current. Do not call the panel validator or scan values/inputs/proofs/labels.
One selected FQ1 coverage aggregate and FQ2 frozen summaries suffice. Reads
of arbitrary JSON, errors, source URLs or user contact values are prohibited.
Blocker counts/status only are sufficient; never echo unknown blocker text.
Manifest lengths/counts may be observed without transferring whole JSON.
Candidate flags must be named recorded research flags. No new ready boolean,
production eligibility, significance or alpha certification conclusion.

Preserve existing read-only snapshot, bounded memory/spill, safe output and
exclusive filenames. No external requests, API/model spending or DB access.
No implicit schema migrations. Tests use tiny isolated DuckDB tables with
256MB/one thread, never initialize the full production schema. Cover absent
and partial schemas, incomplete manifests, snapshot/lineage mismatch, bounded
output, no wide scans/unsafe text, and unchanged certification in JSON/Markdown.

Static edits only until parent grants the sole runtime slot. Focused checks
once, scoped Ruff, one independent review; fix Important on report, rereview
only Critical. Parent will run or authorize the exact Windows process-tree
guard with1.5GiB cap. Shared tree: no stash/reset/restore/checkout/clean;
pathspec-only commit with prescribed coauthor trailer only after parent OK
to integrate. Write implementation report in fundamental-readiness-result.md.
