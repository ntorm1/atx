# Quant-platform swarm status (stopped 2026-09-22 on user request)

Base (Lane 0 scaffold): `334a7939` on `feat/quant-platform-swarm-20260922` (pool-1).
Workflow `wf_8ae59f61-3b6` stopped mid-run. Nothing merged. Reviews/fixes not completed for any lane.

| Lane | Worktree | Branch | Commits | Builder | Uncommitted | Review |
|---|---|---|---|---|---|---|
| 1 Rolling/CS kernels + StreamingEngine | pool-2 | `feat/qps-l1-kernels` | 1 | INTERRUPTED | yes | not run |
| 2 VM subtree cache, fusion, Cs pool, strategy-B CSE, bench gate | pool-3 | `feat/qps-l2-vmcache` | 4 | INTERRUPTED | yes | not run |
| 3 Search throughput: rewrite, fingerprint, fidelity racing, sketch index | pool-4 | `feat/qps-l3-search` | 4 | finished | no | not run |
| 4 Trial registry, FDR/Romano-Wolf/SPA, lockbox, MinTRL | pool-5 | `feat/qps-l4-mtest` | 6 | finished | no | not run |
| 5 Signal-space zoo combiner, orthogonalize, LW2020, HRP/NCO, walk-forward | pool-6 | `feat/qps-l5-combine` | 7 | finished | no | not run |
| 6 Optimizer: cost terms, ADMM schedule, discretize, GP Riccati | pool-7 | `feat/qps-l6-optim` | 3 | INTERRUPTED | no | not run |
| 7 Risk model: fundamental factors, hybrid APCA, validation, attribution | pool-8 | `feat/qps-l7-riskmodel` | 2 | INTERRUPTED | yes | not run |
| 8 E2E zoo->book: replay cost, borrow, events, PreferenceSource | pool-9 | `feat/qps-l8-e2e` | 3 | finished | no | not run |

Per-lane detail: `C:/atx-wt/pool-N/SWARM_STATUS.md`. Worktree leases remain held (release with `scripts/lease-worktree.ps1 -Release pool-N -RunId qps-<lane>`).

## Update 2026-09-23 — merge + sync + wave 2
- Integration branch `feat/quant-platform-swarm-20260922` @ `8c42689a` = Lane 0 + lanes 3,4,5,8 (clean merges).
  Tests on integrated tree: factory 287/287, eval 188/188, combine 174/174, book 88/88, atx-impl 489/490
  (the 1 failure `StageRunSyntheticSmoke.SyntheticSmoke_OnFlagsProducesFiniteScorecard` also fails on C:\atx's existing build — pre-existing).
- Local `main` NOT yet fast-forwarded (ref update denied by permission classifier; needs owner approval).
- WIP of lanes 1,2,7 committed; all 8 lane branches merged with integration (each has lanes 3,4,5,8).
- Wave 2 workflow `wf_a5f33d82-32e`: continue lanes 1,2,6,7; review+fix lanes 3,4,5,8; new lanes
  9 `feat/qps-l9-realmine` (pool-10, real-data DSL mining stage) and 10 `feat/qps-l10-fundzoo` (pool-11, PIT fundamental fields + zoo).
  Specs: SWARM_PLAN.md (lanes 1-8), scratchpad wave2.md (lanes 9-10). Each lane keeps its own SWARM_STATUS.md current.

## Update 2026-09-23 (stop 2)
See committed atx-engine/docs/QUANT_PLATFORM_SWARM_STATUS.md on the integration branch (head 2149c092). Lanes 1-5,7-10 merged; lane 6 unmerged (fix-required).
