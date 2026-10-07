# handoff-D3W: walk-forward fit, v3 weights runner, fitter post-step, ALCOMB-6/-7, D2-9, D2R-3 (R8 close)

Lane D3W, worktree `C:/atx-wt/pool-23`, branch `feat/p9-d3w-20261005`, created at `899b5811`, R8 resume at
`23e650e6`. Items 1-8 of r7/r8 are DONE; the report is `task-D3W-report.md` (beside this file). Head and the
root merged last: see the report's table (updated at the final commit).

## State

- Root merged: `1b64ce45` (`cec719a0`), `fd084e78` (`c7549d6a`); root's later head `685e238b` is docs only and was not merged (root's code head stays `fd084e78`).
- Builds: `p9-d3w-a` .. `-g` Debug (`-f` / `-g` are the MUTATED teeth builds), `p9-d3w-h` Release from
  `c7549d6a` (receipt copied to `d3w-logs/receipt-p9-d3w-h.json`, EXE-CUR all match).
- Object trees `build-equity/` and `build-equity-rel/` were NOT deleted at the close (review S-1); D3W-PB deleted
  their untracked build outputs by name as its first act (2026-10-07; the `mega-p9-d3w-*` receipts and build logs
  copied to `d3w-logs/` first; the tracked `build-equity/audits/` and `v8-interim3-pitch-render-run2/` kept); any
  further C++ test needs a fresh configure and build.
- Tests on the final head: see the report's "Tests (final head)".

## Where things are

- Items, file:line, tests, logs: report section "Items".
- Flag-absent identity procedure and tools: report section "Flag-absent identity"; tools in `d3w-logs/tools/`
  (`ident.py`, `cmp.py`, `d3w-fingerprints.py`; teeth `teeth_py.py`, `teeth_cpp.py`, which hard-code pool-23).
- For PRE / For ABS / For part B: report sections of those names.

## If a fresh agent resumes

1. `git -C C:/atx-wt/pool-2 log --format=%h -1`; if root moved past the merged SHA, merge it (`--no-ff`), then
   re-run the fitter / composition pytest files, `scripts/tests` under seeds 0 and 1, `atx-engine/tools/test_stage_chain.py`
   and, if C++ changed on either side, configure `equity-rel` and rebuild the seven targets of `p9-d3w-h` with a new tag.
2. Re-run the identity (`ident.py` base and head with the same WORK) and record it.

## Open questions

- `scripts/specs/p9/rules/mom-volman-v1.json:50` is stale after ALCOMB-7 (it says `--engine-fit` is refused with
  `--mom-volman`).

## Permission refusals

None.
