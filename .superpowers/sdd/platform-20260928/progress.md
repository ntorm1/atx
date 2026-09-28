# platform-20260928 -- ledger (newest first; `git add -f` this dir)

Goal (owner, 2026-09-28): continue building atx-engine + atx-impl into a production-quality equity L/S quant platform
(high Sharpe, high capacity, low turnover, low/medium frequency). Three fronts: (1) platform + pipeline for faster research
iteration, (2) the implementation and the mega strategy, (3) the pitch to production. Start with code review + web research
-> plan -> 3-4 Opus 5.5 implementer lanes. Root (pool-2) = PM: builds, real-data runs, acceptance, ledger.

Standing rules carried from mega-alpha-20260926 (unchanged): root alone builds (mega-build.ps1) and runs real data
(run_bounded_research.py 180 s / 1536 MiB); children own pool worktrees, never build, never run real data, never spawn
subagents, trailer Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, task-TN-report.md, reply < 15 lines; TRAIN
2020-2022 only; 2023-2024 used twice (new read = validation trial #3, owner gate U1); 2025+ reserved; never read a
per-candidate VAL statistic; no pushes, warehouse writes, broker actions; tree clean before every real-data run; every
ruling declared before any measurement it could bias; Appendix A trial accounting on every TRAIN result.

Inputs: mega-alpha-20260926/{v6-code-review-exec.md, v6-code-review-signal.md, v6-literature.md, v4-prereg.md, progress.md},
docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md, docs/plans/2026-09-28-mega-alpha-data-request-atx-db.md,
docs/plans/2026-09-28-mega-alpha-v6-pitch.html (the v6.1 pitch), final cell mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247
(S2 net +1.239, DSR N29 .911 < .95: freeze gate unmet).

## 2026-09-28 sprint open
- Ruling: research phase first (P1 code review of platform/pipeline/production gaps, P2 literature on combination,
  construction with costs, risk models, capacity, platform design, new families) -> synthesis plan -> 3-4 implementer lanes
  -- owner asked for review + research before implementation -- cost if wrong: ~1 h of agent time before code moves.
- Ruling: research agents read-only, write to this dir; no measurements in this phase -- nothing to bias.

## 2026-09-28 P1 landed; root A5 started
- P1 code-review-v7.md (395 lines) delivered. Headline: v6.1 ladder = 234 s compute inside a ~90 min cycle; SHA-256 is
  30/76 s of u and 19/31 s of w; one new field invalidates 35/39 cache entries; research exes are Debug; no daily decide
  path; risk model + QP + GP aim exist unused in atx-engine/risk. Lanes proposed: L1 IC cache keys + fast hash, L2
  research-cycle driver + field reuse + fit store, L3 daily decide path (holdings emit, decide verb, deploy manifest),
  L4 DSL ops. Root: A5 Release A/B, C3 canary, disk GC.
- Ruling: A5 = build atx-equity-strategy-ic/targets under preset equity-rel (tag v7-rel0), then A/B the v6.1 u pass and
  the final NAV cell for byte identity; adopt Release only if identical -- identity re-run of an existing book is not a
  new trial (same book, same window) -- cost if wrong: one wasted build + two 180 s runs.
