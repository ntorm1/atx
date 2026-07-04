# Sprint 3 progress ledger — Gârleanu-Pedersen Aim-Portfolio Trading

**Goal:** behind an inert `--gp-trading` flag, replace the position-mode deploy's crude linear
partial-trade (`w := prev + trade_rate·(target − prev)`) with the FROZEN GP aim-portfolio trade
(`risk::gp_aim_and_value` + `risk::gp_turnover_native_step`) so the live book trades toward the
risk-curvature-aware GP AIM. Zero new estimator math; the GP functions are called, never edited.

- **Worktree:** `C:\atx-wt\p9`
- **Branch:** `feat/p9`
- **Base:** `main @ c7c7b44` (S1+S2 already landed on this branch: tip `52ac04f`)
- **Build gate:** `powershell -File <scratch>\p9-build.ps1 -Target atx-impl-tests` then
  `powershell -File <scratch>\p9-ctest.ps1 -R <Suite>` (self-contained MSVC-env wrappers).

One line per unit (ROADMAP §141). Newest last.

| Unit | Commit  | Deliverable                                                                                  | Review |
|------|---------|----------------------------------------------------------------------------------------------|--------|
| S3-0 | (kickoff) | ledger opened; `RunConfig::{gp_trading,gp_risk_aversion,gp_trade_cost_scale}` + CLI arms   | —      |
