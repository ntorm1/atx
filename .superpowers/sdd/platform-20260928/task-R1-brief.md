# Task R1: adversarial review of the merged platform-v7 tree (read-only)

**Where:** read C:/atx-wt/pool-2 at HEAD `git -C C:/atx-wt/pool-2 rev-parse HEAD` by absolute path. No edits, no builds,
no runs, no subagents, never touch C:/atx, never read validation/VAL/2023/2024/2025 files. Output:
C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/task-R1-review.md (<= 250 lines) and reply < 10 lines.

**Scope:** the four merged lanes, diff base cdc9c2a8..HEAD: `git -C C:/atx-wt/pool-2 diff --stat cdc9c2a8..HEAD` (C++:
atx-core sha256, atx-impl strategy_ic_runner cache v2, strategy_nav_replay + strategy_live + decide verb, strategy_nav_v7 /
strategy_cost_v2 / strategy_risk_model / risk verb; Python: scripts/research_cycle.py, prepare_research_fields.py --reuse,
studies/backtest_integrity.py + nav_summ.py, fit_composition_weights.py CacheLayout v2, mega_report/pitch.py). Read the
lane briefs (task-L1..L4-brief.md) and reports for intent, and progress.md for what root measured.

**Review for, in this order:** (1) correctness bugs that the identity checks would NOT catch (paths only exercised with
the new flags on: cache v2 store/lookup edge cases, cross-manifest legacy hits with a changed DSL, decide with real
positions differing from the replay, spo/aim-partial-v6 parameter paths, risk model masks, cost v2 formulas vs the brief,
capacity scaling argument, PBO/ONC implementations vs the papers, effective-N variance estimate); (2) look-ahead or
leakage: any place a statistic at date d reads data visible only after d (risk model EWMA windows, bias harness realised
returns alignment, holdings observer timing, decide as-of row); (3) determinism (thread ordering, unordered containers in
output paths, float summation order); (4) silent-NaN or silent-skip paths that should refuse; (5) schema/version bumps
missing (sidecar v2, manifest blocks, decision.json); (6) test gaps vs the briefs' acceptance lists; (7) house-style
violations in .agents/cpp/agent.md terms that matter (ownership, error handling), not formatting.

**Format:** table (id, severity I/M/m, file:line, one-line finding, one-line fix), then details for every I and M with the
exact code cited. Severity I = wrong number or crash on a supported path; M = wrong in an edge case or a missing refusal;
m = everything else. No praise, no restating what works.
