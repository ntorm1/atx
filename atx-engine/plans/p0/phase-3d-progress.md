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

| p3d-B1 | c38169c | imm[2] + Node.hparams/n_out + NodeKey.imm_bits | Alpha 411 |
| p3d-B2 | 097be98 | SlotPool contiguous block acquire/release | SlotPoolBlock 2/2; 412 |
| p3d-B3 | 9507527 | PinSig + OpSig.pins/n_hparams + Pin/Split2 + split2 builtin | 412 |
| p3d-B4 | f8472f7 | lexer Dot token + dotted-field bridge | AlphaLexer 57; 414 |
| p3d-B5 | c693643 | Member AST + postfix .pin parselet + hparam peel | 421 |
| p3d-B6 | 2b1d336 | record TypeInfo + analyze_member + misuse errors | AlphaMember 7/7 |
| p3d-B6b | 2d6e969 | extract reject_record_operands; analyze_call ≤60 | 421 |
| p3d-B7 | 02794f4, 4457e39 | DAG Member→Pin (real index) + Call n_out/hparams/imm_bits; extract lower_member + assert | AlphaDag; 422 |
| p3d-B8 | 73647b9 | linearize multi-output block alloc + Pin emit + block Free | AlphaBytecode; 422 |
| p3d-B9 | c9f8d8d, 79f887a | VM+oracle Split2 + Pin projection (differential); Free comment + relaxed peak guard | MultiOutput; 426 |

## Phase status
- [x] A — local bindings + references (A1–A3) — spec✓ quality✓, 410/410 green
- [x] B — multi-output IR + records + member access (B1–B9) — spec✓ quality✓, 426/426; multi-output proven end-to-end on split2 via bit-exact differential
- [ ] C — strided recurrence + kalman_level + ou_filter (C1–C5)
- [ ] D — Chan kalman record {alpha,beta,resid} (D1–D4)
- [ ] E — OU rolling family (E1–E4)
- [ ] F — integration + docs (F1–F2)
