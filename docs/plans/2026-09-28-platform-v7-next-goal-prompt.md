# Goal prompt for the next parent agent (copy into /goal)

/goal Continue the platform-v7 sprint as project manager, from docs/plans/2026-09-28-platform-v7-handoff-1.md
(read it first, then .superpowers/sdd/platform-20260928/progress.md top 120 lines and v7-prereg.md). Objective: push v7
to completion and produce the v7 scorecard + pitch iteration 3. Work only in C:/atx-wt/pool-2 (branch
feat/aes-codex-integration-20260925); never mutate/build/switch/commit in C:/atx (read-only ok; the atx-db session owns
C:/atx and atx-db: no locks, never kill its processes). Root alone builds via `powershell -File
build-equity/mega-build.ps1 -Tag <new> -Targets "<t1,t2>"` (next tag v7-3; check build-equity/mega-<tag>-receipt.json
does not exist). Root alone runs real data under `"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py
--seconds 180 --max-rss-mib 1536 --min-free-mib 512 --output build-equity/<new-dir> --bind <manifests> -- <cmd>` or
through `scripts/research_cycle.py run specs/<spec>.json`; the tree must be clean (untracked included): `git add -f
.superpowers/sdd/platform-20260928 && git commit` before every run. Children (Opus 5.5): own pool worktree (3, 4, 7, 8,
9, 10, 11; never 1 or 6), never build, never run real data, never spawn subagents, trailer `Co-Authored-By: Claude Opus
5.5 <noreply@anthropic.com>`, task-TN-report.md, reply < 15 lines. TRAIN 2020-2022 only; 2023-2024 used twice (any new
read = validation trial #3, needs owner gate U1); 2025+ reserved; never read a per-candidate VAL statistic. No pushes,
warehouse writes or broker actions. No TDD; post-implementation tests; tasks accepted on root real-data measurements.
RAM limits stay 180 s / 1536 MiB. Every ruling: "Ruling: <decision> -- <why> -- cost if wrong: ...", declared before any
measurement it could bias; pre-register every new cell/library in v7-prereg.md before running it; report every TRAIN
result with the Appendix A trial-accounting block (TRAIN construction cells currently 33; validation trials 2 spent).
Sequence: (1) pick up the in-flight lanes (L7 pool-10 library v7.0, W1 pool-3 spo root-cause, P4's
library-v7-wave2-prereg.md); (2) run library v7.0 through research_cycle (specs/v70.json) and judge wave 1 under its
pre-registration; (3) spo trial #2 only after W1's corrected defaults are pre-registered; (4) pre-register and run wave 2
(library v7.1 on fields-v9; mind the 64-field runner limit); (5) role rebuild with atx-db SIC events as a pre-registered
universe trial; (6) scorecard v7 (docs/plans/...-mega-alpha-scorecard-v7.md, same format as v6) + regenerate the v6
report and the PM pitch (iteration 3: add capacity curve, risk bias, report cards, monitor baseline, decide/orders/
reconcile, integrity statistics, trial ledger) and publish the pitch as an artifact; (7) quiet-host Release A/B; (8) hand
the owner the merge command for local main and the U1 decision. Use 3-5 Opus 5.5 implementer agents in parallel plus
read-only reviewers; preserve your own context window (briefs as files, agents read them; ledger newest-first).
