# Phase 3d — Progress Ledger

State-space & mean-reversion DSL nodes. Plan: `phase-3d-statespace-dsl-implementation-plan.md`.
Design: `phase-3d-statespace-dsl-design.md`. Worktree: `feat/alpha-statespace-nodes`.

Record one row per landed task: `pNd-X | SHA | summary | tests`.

| Task | SHA | Summary | Tests |
|------|-----|---------|-------|
| p3d-0 | 6f8dd4e | design spec | — |
| p3d-0 | bf32cbc | implementation plan + ledger | — |
| p3d-A1 | 9082359 | local bindings — binding-first identifier resolution | AlphaBindings 1/1; Alpha 406/406 |
| p3d-A2 | 3a961d1 | pin forward/self-reference field-fallback (test-only) | AlphaBindings 3/3 |
| p3d-A3 | 3ed50a7 | shadow warning + warnings out-param on parse_program | AlphaBindings 5/5; Alpha 410/410 |

## Phase status
- [x] A — local bindings + references (A1–A3) — spec✓ quality✓, 410/410 green
- [ ] B — multi-output IR + records + member access (B1–B9)
- [ ] C — strided recurrence + kalman_level + ou_filter (C1–C5)
- [ ] D — Chan kalman record {alpha,beta,resid} (D1–D4)
- [ ] E — OU rolling family (E1–E4)
- [ ] F — integration + docs (F1–F2)
