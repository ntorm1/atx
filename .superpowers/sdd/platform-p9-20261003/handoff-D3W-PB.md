# handoff-D3W-PB: ew-theme-aim-v2 in the C++ fit, the resid reference, the D2-3 deletion; D3W-S (R9 close)

**Head (R10 fix round 1, D3WPB-R1):** fix `90bc1962` (four `retired_by` labels -> `P10`, two comments cite D3WPB-Q1; mirror guard 7 passed, 21 / 8) on review PARTIAL `a0c7427c`, then this records commit; lane code `460f5ba2`, pre-deletion `c044f5f6`; next: the resumed D3W-PB review (D3WPB-SEQ).

Lane D3W-PB (stretch; ruling D3W-PB: merges only if APPROVE at the gate, else P10 whole), worktree `C:/atx-wt/pool-23`,
branch `feat/p9-d3w-20261005`, continued from D3W's APPROVE SHA `f0726acb` (ruling MERGE-SHA). Report:
`task-D3W-PB-report.md` beside this file (items, file:line, tests, teeth, identity procedure).

## State

- Code commits after `f0726acb`: `dcddb01d` (S-1 records), `6da02a0d` (S-9), `02d3e7a3` (S-2, S-4, S-5), `54828cdd` (C++
  ew-theme-aim-v2 + `AimV2.*`), `bd4e160e` (fitter `--engine-fit` for it, S-8), `703ac03a` (S-2's pytest).
- Pre-deletion head = the records commit that adds this file; the D2-3 deletion commit is its single child and the
  branch's LAST commit (`refactor(d3w-pb): D2-3: ...`). Both SHAs are in the PM message.
- Root not merged into the lane (root has not merged D3W; B2-EVAL-KEEP); `git merge-tree` of root `fcb0200e` with
  `f0726acb` and with `703ac03a` is clean.
- Builds: `p9-d3wpb-a` Debug (clean, source `703ac03a`, the evidence), `p9-d3wpb-b` Debug (the one teeth build,
  MUTATED, never evidence). Receipts and build logs copied to `d3w-logs/mega-p9-d3wpb-*`. The untracked build outputs
  of `build-equity/` are deleted by name at the close (2.4 GB; the tracked `audits/`, `v8-interim3-pitch-render-run2/`
  and the `mega-p9-d3wpb-*` receipts / logs stay);
  any further C++ test needs a fresh `atx-build.ps1 configure -Preset equity-dev` and a new tag.
- Tests: report section "Tests". Deferred to the gate (TEST-1): whole targets, `scripts/tests` seeds 0 / 1, the stage
  chain, Release.

## Where things are

- Tools in `d3w-logs/tools/`: `pb-ident.py` + `cmp.py` (flag-absent identity), `d3w-fingerprints.py`, `pb-deletion.py`
  (the deletion as a reproducible edit, apply / restore), `pb-teeth-cpp.py` (the teeth run; restores itself).
- D2-3 scope: report section "Scope as delivered" (what stays for the P10 document builder and why).

## For root (merge order)

1. Merge D3W at `f0726acb` (its own review's checks).
2. Merge the pre-deletion head; targeted checks (report "TRAIN identity procedure", step 4) and the identity (steps 1-2:
   expected 21 / 24 identical, the 3 engine-fit weights files differ only at `provenance.engine_fit.producer_fingerprint`).
3. Merge the deletion commit; re-run `atx-impl/tools/test_composition_resid.py` and
   `scripts/tests/test_no_python_mirror.py` (own invocation); fingerprints and identity unchanged.

## If a fresh agent resumes

Nothing is open in code. A review fix round: configure `equity-dev`, build the four targets of `p9-d3wpb-a` with the
next free tag (`p9-d3wpb-c`), run the named filters and the five fitter pytest files with `ATX_RESEARCH_ADMISSION_EXE`
/ `ATX_RESEARCH_COMPOSITION_EXE` set. History is never rewritten: a fix lands as new commits after the deletion
commit, and the PM rules which SHA root merges before and after its identity run.

## Open questions

- Walk-forward rows of the aim rules use whole-TRAIN gains (only ew-theme-aim-v2 is refused here).
- D2-3's remaining rows need the P10 document builder.

## Permission refusals

None.
