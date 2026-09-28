# Task P1: platform + production code review (read-only)

**Where:** read C:/atx-wt/pool-2 (HEAD b4ebb30c) by absolute path. No worktree needed; do NOT edit, build, run any binary, run
real data, or spawn subagents. Never touch C:/atx or C:/atx/atx-db. Never read anything named validation/VAL/2023/2024/2025.
Write: C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/code-review-v7.md (the deliverable) and task-P1-report.md (<= 15
lines). Reply in chat < 10 lines. You are an Opus 5.5 reviewer; be concrete: file:line, measured numbers from existing
outputs/ledger, effort estimate (S/M/L = < 1 day / 1-3 days / > 3 days of one implementer).

**Context, read first (do not redo what they cover; extend):**
.superpowers/sdd/mega-alpha-20260926/v6-code-review-exec.md, v6-code-review-signal.md, progress.md (top 300 lines),
docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md, docs/plans/2026-09-28-mega-alpha-data-request-atx-db.md,
.superpowers/sdd/mega-alpha-20260926/studies/v6u_train.sh + v61_train.sh (how a research cycle runs today).

**Goal of the platform:** production equity L/S, high Sharpe, high capacity, low turnover, daily portfolios; research
iteration must get faster every sprint. Review the pipeline end to end along three axes and rank findings by
(value to the goal) / (effort):

A. Research iteration speed. Today one library cycle = fields (builder, 700 MiB / 1800 s) -> IC pass under 180 s / 1536 MiB
   (needs 3-4 resumable passes) -> fit -> weighted pass -> NAV replay -> nav_summ. Measure from run dirs in build-equity/
   (summary.json elapsed/rss, *-run*/receipt) how long each stage takes and where memory goes (atx-impl/src/strategy_ic_runner.cpp,
   candidate cache format, train_combined). Propose: incremental/candidate-level caching so adding one alpha costs one alpha
   (not a full pass); a single research-cycle CLI that runs the whole ladder with pins; hot-path memory (per-day matrices vs
   whole-panel loads); parallelism; build time of mega-build.ps1 and test targets; what a one-command add-alpha-and-score
   would need. Also: the DSL evaluator (atx-engine vm.hpp/ts_ops.hpp) op coverage vs what the literature families need (ts
   regressions, cross-sectional regressions/neutralisation on multiple groups, ranks with decay, event windows).
B. Production readiness of the daily portfolio path. What exists for: live daily target generation from the latest data
   (roles are TRAIN snapshots; how does one produce today's portfolio?), portfolio optimisation (aim-partial-v5 is heuristic:
   no explicit risk model, no cost-aware optimiser; find the code in strategy_nav_replay.cpp / equity_allocation.cpp), risk
   model (price-risk-v1 only), borrow/locate, corporate actions, order generation and broker handoff, position reconciliation,
   monitoring/alerting vs TRAIN distributions, config/versioning of the deployed book, reproducibility pins. Name the missing
   modules and the smallest production-grade version of each.
C. Code stress points. Determinism (thread ordering, floating sums), error handling/silent NaNs, memory caps, test coverage of
   the NAV replay accounting identities, duplicated logic between Python tools and C++, schema versioning, the parked
   findings from earlier reviews (p/r/w symbol replace in prepare_research_fields.py, etc.).

**Output format for code-review-v7.md:** S1 one-page executive ranking (table: id, finding, axis, value, effort, files);
S2 per-finding detail with file:line evidence and the minimal proposed change; S3 measured stage timings/memory table from
existing run dirs; S4 "what a 4-lane sprint should build first" (your recommendation, 4 lanes, disjoint files so they can
merge cleanly). Keep it under 400 lines.
