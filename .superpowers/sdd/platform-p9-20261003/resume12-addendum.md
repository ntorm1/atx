# P9 RESUME 12: addendum to the standing contract (PM, 2026-10-15 14:45)

Read `resume4-dispatch.md`, `resume6-addendum.md`, `resume7-addendum.md`, `resume8-addendum.md`,
`resume9-addendum.md` (TEST-1), `resume10-addendum.md` ("Owner rules" verbatim; LEASE-DEAD, HOST-4, HOST-5,
REV-FRESH, TEETH-BIN-23), `resume11-addendum.md` (its "Owner rules" reminders and rulings stand), then this file.
This file wins where they differ; a ledger "Ruling" line wins over everything. Then read your section of
`r12-dispatch.md`, the files it names, and grep every ruling id your section names in the ledger `progress.md`
(the blocks from the lines starting "- RESUME 11" and "- RESUME 12" hold the newest rulings). All files are in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/` unless a pool is named.

## Owner rules (unchanged: `resume10-addendum.md` "Owner rules" is the text; `resume11-addendum.md` the reminders)

The owner's words of 2026-10-15 (the STOP 11 goal text, binding): PM only; Opus subagents for every root, reviewer
and cells step; one root agent at a time in pool-2; TEST-1 (the whole suites, Release builds, canaries, golden and
stage chain run ONCE at the gate; a gate red is attributed by merge commit and returned to its lane, BLOCKED, the
merge not reverted); never build, switch or commit in `C:/atx`; never touch `atx-db/` (A4-DB); no push; never
`git worktree prune`; never touch `main` / `origin/main`; never any lease action; open no data dated 2024-01-01 or
later; never edit an expected hash; never run `atx-impl/tools/holdout_gate.py` except as the OD-3 runbook orders it,
once, by the cells root agent; trial ledger 133 lines `27e40f9f` until a registered P9 cell; configure only as
`powershell scripts\atx-build.ps1 configure -Preset <equity-dev|equity-rel>`; build only through
`scripts/research-build.ps1`, target-scoped, one at a time among our pools (HOST-4, HOST-5); PERM-1 (a refused command
is never retried or routed around: exact command in your final message); never `git checkout -- <file>`,
`git restore` or `git stash` on a working file (TEETH-RESTORE, TEETH-CRLF); pytest with explicit paths, `scripts/tests`
and `atx-engine/tools/test_stage_chain.py` in separate invocations; delete your own regenerable build trees and
scratch by name once your report is committed; never wildcard-delete shared scratch; never delete pinned, reference
or golden artifacts (the D-COV stores `build-equity/p9-m2i-dcov-*`, `p9-m2d-cov-risk-container`); pools 8 and 24
FROZEN (TEETH-POOL); pool-23 `build-equity/` never used, never deleted (TEETH-BIN-23); commit trailer exactly
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; ledger `progress.md` is the PM's (never edit a line; root
commits it as it stands with its docs commits, DISPATCH-COMMIT); long record files by the editor tool, not a Bash
heredoc; `rtk proxy <cmd>` for raw git / grep output; interpreter `C:/Program Files/Python312/python.exe` explicitly;
blind: no 2020-2023 statistic in any record or message, root history as one-line subjects only. A subagent's report
is its own account: root re-proves.

## State every agent needs (PM check, read-only, 2026-10-15 14:40)

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`: code head `06dc9e5a` (the M6b merge of SQL4
`44a76937`, 0 conflicts), docs head `89b9644d`. Tracked changes in pool-2: `progress.md` (the STOP 11 lines and the
RESUME 12 block), `next-goal-prompt.md`; untracked, git-ignored: `status-12.md`, this file, `r12-dispatch.md`
(root M6B-FINISH's step 0 commits them, `git add -f`); untracked, never added: five PNGs under `docs/plans/`.
Known-red set EMPTY. The Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` named skip stays.
Next free root build tag `p9-3f`. Root exes of record: the four `p9-3e` exes (`build-equity/mega-p9-3e-receipt.json`,
source `06dc9e5a`; on-disk SHA-256 not yet re-hashed: M6b item 1) and the others as `handoff-root-m6.md` "Exes
(EXE-CUR)" lists them. Trial ledger 133 lines `27e40f9f`. `build-equity/research-store/` absent in pool-2 (SQL3-2).
Host: free RAM 3.65 of 15.69 GB, C: 60.8 GB free, no build running.

| lane | pool / branch | head (verified 14:40) | state | R12 step |
|---|---|---|---|---|
| waves 2 + 3 (all) | merged | root `06dc9e5a` | complete except M6b's checks | gate after M6B-FINISH |
| SQL4 | 32 `feat/p9-sql4-20261008` | `44a76937` APPROVE (code `c5df575d`) | merged at `06dc9e5a`; checks pending | ROOT M6B-FINISH (items 1-9); SQL4-CUTOFF-2, M6B-SCOPE |
| A4 flips + fix-1 tests | 27 | `87df9e00` | P10 whole (A4-DB, A4-P10-FLIPS, A4-FIX1-P10) | owner by hand if wanted |
| whole-wave review | sprint dir `wave-review-*.md` | `22f99018` | DONE; Required = gate rulings | gate root acts on `r11-dispatch.md` "ROOT GATE: rulings" |
| PRE | sprint dir `p9-prereg.md` | round 2b-i | 2b-ii after the gate | PRE agent (PRE-2BII-AGENT), then GATE-REV |

Every other lane: merged at its APPROVE SHA (status-12 table); pools clean; leases untouched.

## Rulings of RESUME 12 (full text in the ledger; binding)

- **R12-RESUME.** One fresh root agent continues M6b (items 1-9 of `handoff-root-m6b.md`); its step 0 commits the
  PM's files; the gate is dispatched on its hand-back at root's code head.
- **SQL4-CUTOFF-2.** The cutoff is met by the merge `06dc9e5a`. A red in M6b's checks that root attributes to a
  slip is fixed in place (one logged commit); a red root cannot so attribute = the SQL4 merge BLOCKED (not reverted),
  SQL4 P10 at the gate, stated in the gate report; the gate head taken as it stands.
- **M6B-SCOPE.** M6b runs items 1-9 and nothing of the gate. Item 5 case 2 and item 6 within RUN-1 / RUN-2 bounds;
  a case that cannot complete in bounds or that the host cannot admit is recorded "not run in P9 (RUN-1 bound /
  host)", P10 as SQL4-COLD-P10; it does not block the merge.
- **GATE-DISPATCH-12.** The gate = `r11-dispatch.md` "ROOT GATE" sections + `root-gate-brief.md`, a chain of root
  agents through `handoff-root-gate-<n>.md`; steps 0 and 8 DONE; `r12-dispatch.md` "ROOT GATE" names the head and tag.
- **PRE-2BII-AGENT.** A fresh PRE agent writes round 2b-ii after the gate (placeholders only; PRE-40 form) and
  commits its own file once.
- **GATE-REV.** One fresh reviewer reads the gate report (PROCEED / RETURN), then the round-2b-ii prereg; two review
  files; the PM rules both before any cell.

## Context and hand-back

If you run low on context: finish the current step to a committed state (`WIP(<group>): ...` only for a half-written
record, never for a half-run check), write the handoff (done / not done / "Exact next step", file:line), report
DONE_WITH_CONCERNS. Do not start a build or a bounded run you cannot wait for.

Final message to the PM (under 200 words; details go in files): STATUS (DONE / DONE_WITH_CONCERNS / BLOCKED /
BLOCKED-HOST), code head, docs head, build tags, tests with pass / fail counts, each root item holds / fails, files
written (paths), disk freed, any permission refusal (the exact command), open questions one line each. No logs, no
2020-2023 statistic.
