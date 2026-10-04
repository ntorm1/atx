# Goal prompt for the next parent (PM) agent

Paste everything below the line as the `/goal` text.

---

Complete the P9 sprint plan `C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` end to end as project manager.

Start by reading, in order, in `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`:
1. `status-5.md` (state, lane table, gates, next steps, what is open with me)
2. `resume4-dispatch.md` (the standing contract you give every agent), then `progress.md` from "RESUME 4" to the end (every "Ruling" line is binding)
3. `handoff-root-m2e.md` and `handoff-root-m2d.md`, then `wave3-carry.md` (its "PM answers" and "Added after drafting" lines win over the rest of that file)

Root tree: `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Lanes live in pooled worktrees with their own handoff files; resume them from those files with fresh agents, never restart. Do not re-dispatch anything the ledger marks complete.

How to run it:
- You are PM only: write the ledger, briefs and rulings. Never build, run real data or edit code yourself. Use Opus 5.5 subagents (model: opus) for every lane, reviewer and root-integrator step, in parallel wherever lanes are independent, one root agent at a time in pool-2. Hand artifacts over as files to protect your context.
- No test-driven development: implement first, then the tests the briefs name.
- Priority: real engine and implementation progress in atx-engine and atx-impl; keep moving from JSON artifacts and repeated Python scripts to SQLite and core C++.
- Decide, don't stall. Record every decision in the ledger as "Ruling: decision -- why -- cost if wrong" before the measurement it could bias. Stop only for irreversible or destructive actions, security-sensitive actions, pushes or merges outside the worktrees, or a plan so broken every path is a guess.
- Every lane gets a task review; fix rounds until APPROVE; root re-proves each lane from the committed head at merge. Treat a subagent's report as its own account, not as proof.

Hard rules:
- Never build, switch or commit in `C:/atx`. Never touch `atx-db/` (another session runs jobs there). No push. Never `git worktree prune`. Never touch `main` or `origin/main`.
- Open no data dated 2024-01-01 or later. Never edit an expected hash.
- Trial ledger `build-equity/trials.jsonl` is 133 lines (sha `27e40f9f`); it changes only through registered P9 cells.
- Configure only as `powershell scripts\atx-build.ps1 configure -Preset <preset>`; build only through `scripts/research-build.ps1`, target-scoped, one at a time (1.5-3 GB free RAM). If the permission system refuses a command, do not retry it and do not route around it: report it to me.
- Pytest with explicit paths only; `scripts/tests` and `atx-engine/tools/test_stage_chain.py` in separate invocations.
- Disk: agents delete their own regenerable build trees and scratch by name once their report is committed; never wildcard-delete shared scratch; never delete pinned, reference or golden artifacts.
- Subagent commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

Ask me before acting on these (status-5 "Open with the owner"); rule everything else yourself:
1. The accepted book's universe reads 2026-dated inputs through the atx-db identity bridge. No P9 cell and no OD-3 read until I give you the atx-db diff result or accept the exposure in words.
2. The Debug vs Release `pow` difference: keep the visible Release skip, or approve the re-pin fix.
3. Local `main` and `origin/main` (pushed merge, 6 factory reds, broken `dev` preset).
4. Whether P9 gets a new-signal cell (P9-S lapsed at 0).
5. Any data purchase.

Done means: wave 2 merged with 0 failures, wave 3 and PRE complete, `p9-prereg.md` written and ruled, P9 cells run, adoption print, OD-3 read once, freeze gate passed, and a final list of every ruling made in the sprint. Keep replies to me terse.
