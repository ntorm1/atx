# Wave 0 data build brief (root only; bounded runner; TRAIN 2020-2023)

Read first, binding: `integrator-rules.md`; then `w0-2-runbook.md` (R1-R14, sections 1, 2, 4, 5) and
`w0-2-runbook-open-questions.md`; `v8-prereg.md` (rulings W0-a..c); `task-W0-1-report.md`, `task-W0-2-tool-report.md`.
You are the only process that touches root. Tree clean before every run (commit first; sprint dir with `git add -f`).
Hidden-data rule absolute: nothing dated 2024-01-01 or later is opened by you or by any tool you run; the reader-side
seal does the dropping. If a step would open such a file, stop that step and report it.

## Preconditions (do, record in the log)
1. Free disk on C: >= 30 GB (`df -h /c`). Through B0c about 23 GiB is needed. If below 30 GB, STOP and report.
2. The section 1 checks of the runbook (SEAL_DATE prints 2024-01-01 from research_window; the four SEAL constants).
3. The six blockers in runbook section 0 at the current head: for each, either show it is already fixed at head
   (file:line) or fix it in place with the smallest change (one-line seal guards; constants read from
   `research_window`), commit `fix(<area>): ... (W0 precondition)`. Blocker 1 (regsho_threshold stage republished):
   re-hash the live stage manifests immediately before R8/R10/R11 and record the digests; the fields v9 build pins
   the live manifest (no reuse from the old fields dir is possible anyway).
4. IC and NAV executables are from the latest tracked build (integration 5 part C's tag or later); record the
   build receipt digests. If part C's build is older than the last source change, build first (tag prefix `v8-9`).

## Caps (Rulings W0-c, E-18, E-24, E-28)
IC phases: 300 s / 2,560 MiB, 4 workers. Preparation steps (projection, roles, repair, bridge, events, fields):
600 s / 2,560 MiB. Everything else 180 s / 1,536 MiB unless the runbook states a measured larger need (then use the
runbook's number and record it). A refusal on the free-memory floor: wait two minutes, retry up to five times; then
record and stop.

## Steps (THIS DISPATCH: R1-R9 only; stop after R9, report)
R1 .. R12 of the runbook in order, one bounded run each (`scripts/run_bounded_research.py`), the exact argv from the
runbook's section 3 (reconstructed argv in R10/R11). After each run: receipt ExitCode, seconds, peak MiB, output
manifest sha256 into the log. Commit before the next run.

R13 (cold u pass v7.1 on lo1 and the overlap reports) under Ruling W0-a, in this order: (1) field overlap report
(fields v9 lo1 against the v7.1 fields on the common 2020-2022 cells), (2) signal overlap, (3) daily IC overlap.
Read each report's `bit_identical` and `max_abs_diff` before the next. Outcome classes: bit-identical -> continue;
difference below 1e-9 -> disclose in the log and continue (the 4-year values become the reference); larger ->
STOP the re-base, record the first differing cell, do not run the next report. The overlap tool was fixed this
session (C-9, C-10): zero compared cells is never identical; NaN-vs-value is a mismatch. A report with zero cells
compared is a finding, not a pass.

R14: pins into `v8-prereg.md` (research_window.json, role lo1 and lo3 manifests, fields v9 lo1 / lo3 manifests);
the chained `protocol` ledger line (kind protocol, adds 0 to N, carries the hash chain head; `backtest_integrity`
validates it); `lock --write` on `scripts/specs/v8/base-lo1.json` and `base-lo3.json`; commit.

Do NOT run R15 (delisting-returns role) and do NOT run any cell (W0-4 / B0a); those are later dispatches.

## Report
Append a section "Wave 0" to `integration-log.md`: table of R1-R14 (argv digest, seconds, peak MiB, output sha256),
the three overlap outcomes with their numbers, the pins written, disk before and after, every fix made. Commit.
Final reply at most 12 lines: head SHA, each R step pass / fail, overlap classes, disk after, open items.
