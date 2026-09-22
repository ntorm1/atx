# S4 T11 review corrections

Review: `task-11-review.md`, two Important findings and no Critical findings.
No re-review is required under the program ruling. This correction edits
documentation only; no warehouse, runner, memory guard, activation/jobs, registry,
or source archive was changed or launched.

## T11-R1: current resource envelope

- The runbook now overrides historical CLI defaults with 1 GB/one DuckDB thread,
  one heavy process tree, and the reviewed guard's at-most-4-GiB aggregate commit
  cap plus physical/commit headroom checks. It says explicitly that DuckDB's
  setting does not limit pandas and a job cap is not proof a stage can complete.
- Every activation/resume/selected-stage example uses the exact versioned guard
  path from the `atx-db` directory and a distinct receipt name. Sixteen
  reconciliation shards execute sequentially, one shard child at a time.
- The AR6 publish and optional source-audit handoff now use the same guard and
  1 GB/one-thread setting. README refresh/publication examples use those lower
  settings and direct this rebuild to the guarded launch procedure.
- The local test lane is `-n 0`; four-worker CI remains a separate runner policy.
  Guard version `0e6737fa` is cited without repeating or overstating the bounded
  probe: the independent reviewer found no blocker in that guard.

## T11-R2: scoped truth about determinism

- Removed whole-ingestion claims that no path reads the clock or uses load-time
  ordering, and removed the promise that pinning a network observation date
  guarantees byte-identical downloads.
- Retained only the inspected factor-panel duplicate rule and factor-breadth
  availability contract. Remote revisions and modeled availability stay explicit.
- Documented the AR6 plain-loader correction: only unique existing vendor links
  are reused; competing links use stable vendor keys, and positive collision keys
  do not depend on display ticker.
- Named the remaining bulk `_create_symbol_map` use of `current_date` and
  `source_loaded_at`, and assigned it to the historical identity follow-on.
  Source-vintage uncertainty is not presented as a determinism guarantee.

## Remaining review checkpoint edits

The runbook references `docs/TIER1_ACTIVATION_STATUS.md` for measured run4
ingestion and the statement-stage memory failure rather than copying its
inventory. It says run4 stopped and requires checking for a replacement writer.
S3's 18-module retirement is recorded as landed in `72d38a93`, without claiming
production activation. The source TSV, archive, sidecar and backups are retained;
cleanup needs a separate controller retention decision.

Validation: dictionary generator `--check` passed (definitions/generated output
were unchanged); `git diff --check` passed. PowerShell's parser accepted all
four runbook and two AR6-report command blocks without execution. Static
inspection confirms the guard path and bounded arguments. No documentation
test, full suite, guard probe, source scan or production command was rerun for
these reversible prose/command-example edits.
