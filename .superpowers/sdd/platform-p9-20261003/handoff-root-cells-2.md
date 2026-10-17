# Cells root agent 2 handoff: P9-L ACCEPTED and ledgered; STOP before H

Pool `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`, 2026-10-17. No subagents, builds or suites.

**Final amendment:** fresh DERIV `86fcd5df` agrees; PM ACCEPTED and released the ledger step.
The historical DERIV checkpoint below is complete. See the final next-step section at the end.

## Done

- Restart verification `89b8ae72`: 27/27 frozen exes on each build; references/CRT/ledger held.
- Resource-only specs `f560f741`; PRE-40/plan `7a49c825`; saved preflight `89477326`;
  exact registration-diff supplement `9e17644f`.
- Owner stopped RAM waiting; helper PID 14312 stopped, log preserved. R15-RAM-NOW committed
  `f6512b51` before immediate w2. No admission-wait or host-budget option; caps/floor retained.
- w2 and single calibrated NAV completed; mechanics 6/6 pass, matched gross, no L correction.
- Generated plain record spec `ac903247`, SHA `de44a668999ab8997c7118afb52efd93551ac2e7df93efe9118edbf719aa9a4d`.
- Own ACCEPTED mechanics derivation `7bceff42` BEFORE registered return reads.
- Rho, bundle, orientation/admission/IC diagnostics and Appendix A completed; record
  `cells/p9l-label-terminal-record.md` sections 10-11. Every hand seal scan OTHER 0.
- Cache unchanged since run1 (686 rows); frozen exes/reference dirs/CRT reverified at end.

## Not done

Fresh DERIV, PM agreement/ruling, P9-L monitor and trial-ledger summ; no P9-H work.
No adoption/freeze/OD-3 work. Prepared PM future briefs were committed as requested, not executed.

## Exact next step

PM dispatches fresh DERIV, blind to this handoff and root record until its own derivation is committed.
Use `cells/p9l-label-terminal-plan.md`, prereg B.1, and output files. Plain record planned argv:
`cells/logs/p9l-plan-label-terminal-gm.txt`; actual one NAV receipt:
`build-equity/mega-nav-p9l-label-terminal-gm-run/receipt.json` (calibration flags correctly present).
No second NAV or separate -cal output directory. R15-RAM-NOW governs any future stages: immediate supported
launch, registered limits retained, no RAM waits or automatic retry after an actual resource stop.

Only after PM says DERIV agrees: plan section 3 step 9 through the cal cycle (without admission-wait),
monitor then summ/trial append; verify ledger and hand back before next cell. Next parent's candidate is
`scripts/specs/p9/templates/label-terminal-gm.json`, pending that ruling.

Ledger unchanged throughout: 133 lines,
`27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
Exe table unchanged: Release p9-3f / Debug p9-3g, source `5f48a25d`; re-pin `2078f877` remains.
Full hashes: `cells/logs/exe-sha-{rel,dbg}-2-end.txt`; CRT remains 10.0.26100.7623,
`2C08E77F86B0953CFFA505BDBE64E87ECB7C088DB65D5ACB9A2EEE65A32565EF`.
All raw logs and outputs preserved; five owner PNGs untouched. No permission refusal.

## Final completion and next root (supersedes the pending-DERIV text above)

PM R15-DERIV-SCOPE accepted the independent reader's explicit scope limitation; PM acceptance and ledger
release committed `f6a94011`. Root ran cal cycle to monitor, sealed it (OTHER 0), then cal cycle to completion
(summ, seal OTHER 0), under R15-RAM-NOW with unchanged 180 s / RSS 1536 MiB / free floor 512 MiB.
All completed earlier outputs were reused. No return statistic from monitor/summ was read.

Receipts (both completed exit 0):
- `build-equity/mega-monitor-p9l-label-terminal-run/receipt.json`
- `build-equity/cycle-p9l-label-terminal-gm/summ-run1/receipt.json`

Exactly one construction count-1 trial appended: `4a8141373c779aa1`.
**Final ledger: 134 lines, SHA `b5eeab30594960c6bb52af2366b7a6d5d3ab90b0c84b13a05de0ab14a015ae61`.**
The first 133 lines retain full baseline SHA `27e40f9f6314585e5ff002ad5e3eacdcdb34e8960d9dc62111074487b34d8554`.
N_c 63, N_hand 124, M 110, N_tot 234, P9 admission additions 0. Audit:
`cells/logs/p9l-ledger-completion.json`; output hashes `cells/logs/p9l-completion-outputs.txt`;
record section 12 and `cells/logs/p9l-accounting-after-ledger.txt`.

Exact next step: fresh root 3 receives PM release for **P9-H `p9h-cov-hedge`**, over parent
`scripts/specs/p9/templates/label-terminal-gm.json`, paired to `build-equity/mega-nav-p9l-label-terminal-gm`.
Prepared dispatches `r14-prompts/cells-chain-3.md` and `cells-prompt-3.md` were committed as PM docs;
root 2 did not act on them. First v2 H must capture **G-P1 end-to-end timing/integration evidence**.
R15-RAM-NOW remains: no RAM waits; immediate supported route, retained registered limits; actual resource
stop means preserve evidence and report, no automatic retry. Nothing of H is started. Root 2 stops here.
