# Goal prompt for the next parent (PM) agent

Paste everything below the line as the `/goal` text. The paragraph "Decisions already made for me" carries the
delegation the owner gave ("do not stop to ask questions, you are allowed to rule on all open questions") and the
later note on parallel agents; edit it before pasting if either should change. Two rulings there (B2-14, PERM-1)
concern the permission system: strike or change them if you disagree.

---

Complete the P9 sprint plan `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` end to end as project manager.

Start by reading, in order, in `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`:
1. `status-7.md` (state, lane table with every stopped lane's head, gates, next steps, what is open with me, the open questions with the last PM's recommendations)
2. `resume4-dispatch.md`, `resume6-addendum.md` and `r6-wave3-common.md` (the standing contracts you give every agent), then `progress.md` from "RESUME 6" to the end (every "Ruling" line is binding, in that block and in the earlier ones)
3. `handoff-root-m2j.md` and `handoff-root-m2i.md`, then `root-wave2-gate-report.md` item 4 (d)
4. `p9-prereg.md` (round 2a; Appendix IV lists the round-2b placeholders; rulings PRE-32 .. PRE-39 answer its last section and are not in the text yet) and `p9-prereg-flags.md`, then `wave3-carry.md` (its "PM answers" and "Added after drafting" lines win over the rest of that file; status-7's wave-3 table adds the later carries)

Root tree: `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Lanes live in pooled worktrees with their own handoff files; resume them from those files with fresh agents, never restart. Do not re-dispatch anything the ledger marks complete. First verify the stop state yourself (heads, clean trees, trial ledger, lease status), read-only.

How to run it:
- You are PM only: write the ledger, briefs and rulings. Never build, run real data or edit code yourself. Use Opus 5.5 subagents (model: opus) for every lane, reviewer and root-integrator step. Run as many lanes in parallel as are independent (eight or nine at once worked); one root agent at a time in pool-2. Hand artifacts over as files to protect your context.
- No test-driven development: implement first, then the tests the briefs name.
- Priority: real engine and implementation progress in atx-engine and atx-impl; keep moving from JSON artifacts and repeated Python scripts to SQLite and core C++.
- Decide, don't stall, and do not stop to ask me questions: you may rule on every open question. Record every decision in the ledger as "Ruling: decision -- why -- cost if wrong" before the measurement it could bias. Stop only for irreversible or destructive actions that I have not asked for, security-sensitive actions, pushes or merges outside the worktrees, or a plan so broken every path is a guess.
- Every lane gets a task review; fix rounds until APPROVE; root re-proves each lane from the committed head at merge, and runs the whole `scripts/tests` at every merge (ruling ROOT-1). Treat a subagent's report as its own account, not as proof.

Hard rules:
- Never build, switch or commit in `C:/atx`. Never touch `atx-db/` (another session runs jobs there). No push. Never `git worktree prune`. Never touch `main` or `origin/main`.
- Open no data dated 2024-01-01 or later. Never edit an expected hash. Never run `atx-impl/tools/holdout_gate.py` (it reads 2024+).
- Trial ledger `build-equity/trials.jsonl` is 133 lines (sha `27e40f9f`); it changes only through registered P9 cells.
- Configure only as `powershell scripts\atx-build.ps1 configure -Preset <preset>` (lanes use `equity-dev` / `equity-rel`; the `dev` preset does not configure on this branch); build only through `scripts/research-build.ps1`, target-scoped, one at a time (1.5-3 GB free RAM). If the permission system refuses a command, do not retry it and do not route around it: report it to me.
- Pytest with explicit paths only; `scripts/tests` and `atx-engine/tools/test_stage_chain.py` in separate invocations.
- Disk: agents delete their own regenerable build trees and scratch by name once their report is committed; never wildcard-delete shared scratch; never delete pinned, reference or golden artifacts (the two D-COV stores under `build-equity/p9-m2i-dcov-*` are pinned).
- Subagent commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

Decisions already made for me (ledger OD-DELEG, OD5-1 .. OD5-7, and the rulings of RESUME 6; binding unless I say otherwise in this session):
1. Identity bridge: the exposure is accepted. P9 cells and the single OD-3 read run on the universe as built; every print carries the flag; a freeze pass is worded "passed with the identity-bridge flag".
2. Debug vs Release `pow` / `exp2`: the visible Release skip stays; no re-pin in P9. Release is the producer of record; the two Release-built covariance stores are pinned and never rebuilt.
3. Local `main` and `origin/main`: untouched; listed for me in the final report.
4. No new-signal cell (P9-S lapsed at 0). No data purchase.
5. G-B2 hurdle 8.0%, labelled "plan placeholder, not owner-stated". P9-X is the Y-1 form (vol-target, cap 2.0); it is not a new trial if no P9 cell is accepted before it.
6. Parallel lanes: a wave-3 lane may start on a dependency head that is APPROVE before root has merged it (W3-EARLY-2); it merges root's head again before its report.
7. Permission system: B2-14 (a merge conflict is resolved region by region with ordinary edits, never by a wholesale checkout, restore or copy-over) and PERM-1 (a refused command is never retried or replaced by a same-effect substitute; only the form the refusal's own message names may be used, once, and is reported).

Unfinished housekeeping of mine (status-7 "Open with the owner"):
- `C:\atx\data`: delete what atx-engine and atx-impl do not currently use. Only a read-only survey exists (ledger line "OWNER INSTRUCTION ... C:\atx\data"). Do this ONLY if this line is still in the prompt: finish the classification first; delete only entries shown unused and regenerable or superseded; name vendor, source and history-block data, and anything an atx-db job may read, to me instead of deleting it; never open the entry dated 2026. If a deletion is refused, report it.

Done means: wave 2 merged with 0 failures and its gate run, wave 3 merged and its gate run, `p9-prereg.md` completed (round 2b), reviewed and ruled, P9 cells run in the registered order, adoption print, OD-3 read once, freeze gate verdict written, and a final list of every ruling made in the sprint. Keep replies to me terse.
