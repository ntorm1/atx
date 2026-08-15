# atx-vol ledger — append-only durable facts

Protocol: one line per fact, newest last. Format: `YYYY-MM-DD | area | fact | source`. Never edit or delete existing lines — a correction is a NEW line citing the line it supersedes. Grep this file before re-deriving build traps, measured numbers, or decisions. Append only facts worth remembering across sessions: gate results, discovered traps, measured baselines, decisions with rationale. Keep entries one line; long rationale belongs in `sprints/` or `docs/` with a pointer here.

---

2026-07-22 | build | dev-shared preset NEVER for test gate: per-DLL counter globals (atx/vol/counters.hpp) split per image, SolveLedger/BacktestExec observer suites fail deterministically | .agents/cpp/agent.md
2026-07-22 | build | worktree pool measured: warm tree 5 s no-op, 27 s preset/branch flip; fresh worktree 132 s at 38% ccache hits (PCH-consumer TUs never transfer cross-worktree) | .agents/cpp/agent.md
2026-07-22 | build | manual `git worktree add` skips submodules (databento-cpp arrives empty, configure dies); use scripts/lease-worktree.ps1 or new-worktree.ps1 | .agents/cpp/agent.md
2026-08-14 | test | suite composition at 9b07081: 2953 ctest targets labeled atx_vol_fast, 13 bare atx_vol, 275 other (whole repo 3241) — measured, not invariant | claude-mem obs 42606/42608
2026-08-14 | release | 1.1.0 shipped on feat/vol-derivs: SurfaceOverlay new (aggregate-arity static_assert pin), scenario_grid I-4 ratio retired as NOT REPRODUCED | CHANGELOG.md 1.1.0
2026-08-15 | harness | vol DAG harness v1 installed: CLAUDE.md spine (root + atx-vol), .claude/agents/vol-{planner,builder,reviewer,verifier}, .claude/workflows/vol-sprint.js, vol-dag skill, this ledger | docs/superpowers/specs/2026-08-15-vol-dag-harness-design.md
2026-08-15 | harness | oracle RSI loop installed (spec approved): vol-analyst agent, vol-oracle-iter workflow, oracle_ingest.py, cohort rules, NORTHSTAR dashboard, bootstrap charter; not yet run — data not ingested, bench tool not built | docs/superpowers/specs/2026-08-15-oracle-rsi-loop-design.md
2026-08-15 | data | SpiderRock oracle drop: tbloptionintradayhist_30min_eqt_id_v2.00_2026-08-14.zip (Downloads, 2.45 GB -> 15 GB TSV, ~33M rows); srPrc/srVol/greeks AND their inputs (rate sdiv ddiv years uPrc) per row -> Mode A/B error decomposition possible; -99 sentinels; date col UTC (13:30 = dropped 9:30 ET slice) | spec §1
