# Goal prompt for the next parent (PM) agent

Paste everything below the line as the `/goal` text. The paragraph "Decisions already made for me" carries the
delegation the owner gave in the last session ("do not stop to ask questions, you are allowed to rule on all open
questions"); edit it before pasting if that should change.

---

Complete the P9 sprint plan `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` end to end as project manager.

Start by reading, in order, in `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`:
1. `status-6.md` (state, lane table, gates, next steps, what is open with me)
2. `resume4-dispatch.md` (the standing contract you give every agent), then `progress.md` from "RESUME 5" to the end (every "Ruling" line is binding, in that block and in the earlier ones)
3. `handoff-root-m2h.md` and `handoff-root-m2g.md`, then `root-wave2-gate-report.md`
4. `p9-prereg.md` (round-1 draft; its section H is answered by rulings PRE-1 .. PRE-27 and PRE-7a in the ledger), then `wave3-carry.md` (its "PM answers" and "Added after drafting" lines win over the rest of that file; status-6's wave-3 table adds this session's carries)

Root tree: `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Lanes live in pooled worktrees with their own handoff files; resume them from those files with fresh agents, never restart. Do not re-dispatch anything the ledger marks complete.

How to run it:
- You are PM only: write the ledger, briefs and rulings. Never build, run real data or edit code yourself. Use Opus 5.5 subagents (model: opus) for every lane, reviewer and root-integrator step, in parallel wherever lanes are independent, one root agent at a time in pool-2. Hand artifacts over as files to protect your context.
- No test-driven development: implement first, then the tests the briefs name.
- Priority: real engine and implementation progress in atx-engine and atx-impl; keep moving from JSON artifacts and repeated Python scripts to SQLite and core C++.
- Decide, don't stall, and do not stop to ask me questions: you may rule on every open question. Record every decision in the ledger as "Ruling: decision -- why -- cost if wrong" before the measurement it could bias. Stop only for irreversible or destructive actions that I have not asked for, security-sensitive actions, pushes or merges outside the worktrees, or a plan so broken every path is a guess.
- Every lane gets a task review; fix rounds until APPROVE; root re-proves each lane from the committed head at merge, and runs the whole `scripts/tests` at every merge (ruling ROOT-1). Treat a subagent's report as its own account, not as proof.

Hard rules:
- Never build, switch or commit in `C:/atx`. Never touch `atx-db/` (another session runs jobs there). No push. Never `git worktree prune`. Never touch `main` or `origin/main`.
- Open no data dated 2024-01-01 or later. Never edit an expected hash. Never run `atx-impl/tools/holdout_gate.py` (it reads 2024+).
- Trial ledger `build-equity/trials.jsonl` is 133 lines (sha `27e40f9f`); it changes only through registered P9 cells.
- Configure only as `powershell scripts\atx-build.ps1 configure -Preset <preset>`; build only through `scripts/research-build.ps1`, target-scoped, one at a time (1.5-3 GB free RAM). If the permission system refuses a command, do not retry it and do not route around it: report it to me.
- Pytest with explicit paths only; `scripts/tests` and `atx-engine/tools/test_stage_chain.py` in separate invocations.
- Disk: agents delete their own regenerable build trees and scratch by name once their report is committed; never wildcard-delete shared scratch; never delete pinned, reference or golden artifacts.
- Subagent commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

Decisions already made for me (ledger OD-DELEG, OD5-1 .. OD5-7; binding unless I say otherwise in this session):
1. Identity bridge: the exposure is accepted. P9 cells and the single OD-3 read run on the universe as built; every print carries the flag; a freeze pass is worded "passed with the identity-bridge flag".
2. Debug vs Release `pow` / `exp2`: the visible Release skip stays; no re-pin in P9.
3. Local `main` and `origin/main`: untouched; listed for me in the final report.
4. No new-signal cell (P9-S lapsed at 0). 5. No data purchase.
6. G-B2 hurdle 8.0%, labelled "plan placeholder, not owner-stated". P9-X is the Y-1 form (vol-target, cap 2.0).

Two housekeeping requests of mine are unfinished (status-6 "Open with the owner", items 1 and 2):
- Unused worktrees: pools 8, 13, 21, 23 are released and checked safe; `git worktree remove` was refused by the permission system. Do not retry it; I remove them myself.
- `C:\atx\data`: delete what atx-engine and atx-impl do not currently use. Only a read-only survey exists. Finish the classification first; delete only entries shown unused and regenerable or superseded; name vendor, source and history-block data, and anything an atx-db job may read, to me instead of deleting it; never open the entry dated 2026. If a deletion is refused, report it.

Done means: wave 2 merged with 0 failures, wave 3 and PRE complete, `p9-prereg.md` written and ruled, P9 cells run, adoption print, OD-3 read once, freeze gate passed, and a final list of every ruling made in the sprint. Keep replies to me terse.
