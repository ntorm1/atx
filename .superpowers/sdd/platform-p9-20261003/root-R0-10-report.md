# Root R0-10 report: X-10, leverage L 2.0 on Y-F0

Status: **DONE.** X-10 ran by hand through `research_cycle.py`, which is the route the registration rules for this cell.
Its scaled mechanics pass 6/6. Under the registered rule, PM7-34 (3), the cell is **ACCEPTED**:
- its net annual return is above Y-F0's;
- its S2 net Sharpe is lower by .0412, inside the .100 the rule allows;
- its mechanics pass.

N goes 60 -> 61.
- Root: `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`. Date 2026-10-03, 14:50Z-15:05Z.
- Rulings applied: SEAL-ALLOW (per-hit check of X-10's own run dirs), R0-7-MAN (as below), R0-7-EOL (done in R0-7),
  OD-P9-3 (L <= 2.0; X-10 at exactly 2.0). R0-6-ATT was not needed: no attempt failed.
- No stop condition fired:
  - every key came from the registration or the chain rule;
  - every pin verified (plan: 6 of 6 `[locked, verified]`; NAV exe `72ff6d2d` = Y-F0's);
  - no validator refused (`lock`, `plan` and `test_research_spec.py` 86 passed);
  - the seal check found 0 OTHER;
  - nothing redid a completed stage: u, fit, card, w and monitor were resumed as done.

## Route: by hand, no `waves/x-10.json` (logged in the integration log, commit `50406181`)

- **PM8-14 rules YP-7:** "Y-S (stage by stage), Y-3, Y-2, Y-5 through `wave run`; X-10 and Y-1 by hand". YP-7 gives
  the reason: the driver "has only `v8-mech` limits and no leverage acceptance rule, and a new named rule is a reviewed
  code change".
- **Checked on disk** in `scripts/wave_rules.py`:
  - `MECHANICS` holds only the unscaled `v8-mech`. An L 2.0 book's gross of about 1.67 would fail its [.90, 1.05].
  - `ACCEPTANCE` holds only `pm7-34` (dSR > 0) and `v8-prereg-5`.
  - `rule_cell` needs a template file, and X-10 has none: its registration is a `change.set` on Y-F0.
- **Result:** a wave manifest would judge X-10 by the wrong limits and the wrong rule, and fixing that is a code change.
  So R0-7-MAN's procedure (every key registered, chained or copied; no free constant) was applied to the form that
  v8y §8 / §14 registers: the cell's template spec.
- **Runbook:** the R-8 hand-cell precedent and `task-CELLS-brief.md`. The bundle and the readers used the driver's own
  argv builders, `wave_steps.bundle_argv` and `reader_argv`.

## Scaled thresholds, committed before any X-10 run

The thresholds were committed in **`50406181`** (`log(p9): R0-10 X-10 plan: scaled mechanics r = 2.0/1.1828 ...`),
before the spec and before any run. r = 2.0 / 1.1828 = 1.6909029421711192.

| row (S2, all rows) | v8-mech | X-10 limit |
|---|---|---|
| mean gross | [.90, 1.05] | [1.521813, 1.775448] |
| abs(mean net) | <= .02 | <= .033818 |
| tau mean | <= .20 | <= .338181 |
| tau p95 | <= .30 | <= .507271 |
| accounting (2 rows) | <= tolerance | unchanged |

Acceptance was written in the same commit:
- net annual return > Y-F0's .056538;
- S2 net Sharpe >= 1.849482 - .100 = 1.749482;
- mechanics pass.

The 4x guard, dSR and both p are printed and decide nothing.

## Per stage

- Host check before every launch: no compiler, no `atx-*` exe, no other research process.
- Memory gate: free >= 3,072 MiB (the 1,536 MiB cap + 1,536). Free memory was sampled every 0.25 s.
- No gate wait was longer than 4 s.

| step | exit | s / peak MiB | free before / min | receipt SHA-256 | commit |
|---|---|---|---|---|---|
| spec `scripts/specs/v8/x-leverage-L2.0.json` (`ebe68e6e`, digest `234dbe71`); `lock --write`, `plan` | 0, 0 | - | - | - | **`42ddf3ba`** (pre-registration) |
| NAV `run --stop-after nav` (L 2.0) | 0 | 49.1 / 586 | 3,101 / 2,065 | `b14ac066b05a0528b9e01ab3cb12e17c67d6608fd191e2925def2df8cdc3fd6b` | - |
| mechanics reader (keys only) | 0 | 0.5 / 45 | 3,498 / 3,389 | `2d933c8890d4377a37fb16ce30ed63cf684848430db0760149746f20ad864fd5` | **`c8391595`** (mechanics PASS 6/6) |
| cycle `run` (summ ledgers the cell) | 0 | 35.5 / 629 | 3,617 / 2,866 | `605b710f556d4aee52e0e63b8c19bdad700b4ac08ee97fbb8c2643cd4ae649e6` | - |
| bundle (PM5-23) | 0 | 0.8 / 561 | 3,459 / 2,820 | `63eae3e7d105a2c5afbd7ad01c8cc5de3b41790da57666e4fcab0965b05eaa4f` | - |
| book reader | 0 | 0.3 / 5 | 3,447 / 3,416 | `28bf0d07f78aee45380e3f2fec7ff111b0bbc2268deb4ea36efbcf4fd5c06c48` | **`e6418b0d`** (verdict) |

- **NAV argv vs Y-F0's receipt:** only `--output` and `--aim-leverage 1.1828 -> 2.0` differ.
- **Output files:**
  - NAV `build-equity/mega-nav-v8x-theme-erc-L2.0-v8ysb`;
  - `cycle-v8x-leverage-L2.0/cycle_verdict.json` `a11bd45a`;
  - `p9-r10-x10/{mech,bundle,book}.json` `603b5afb` / `a57d82d0` / `d222b547`.
- **Seal:**
  - 20 log, receipt and console files, plus the start and binding files, were checked.
  - Every hit was `2026-10-02` source (a) (the png name) or `2026-10-03` source (b) (`started_utc`), apart from the
    bootstrap seeds.
  - **0 OTHER.**

## Verdict and numbers (as printed)

**No tool prints a PM7-34 (3) verdict:**
- `cycle_verdict.json` has no accepted field.
- The bundle's `verdict` block (`pass: false`) is the v8-prereg item 9 freeze-gate part, not X-10's rule.

Root's line, applied by hand:

`verdict (PM7-34 (3), v8y 6 / 8, by hand): ACCEPTED {net_annual_above_parent: True (.093061 > .056538),
sharpe_within_.100: True (1.808289 >= 1.749482; dSR -.041193), mechanics: True (6/6 at r 1.690903)}`

**Mechanics:**
- gross 1.666718; abs net .010446; tau .028189 / p95 .033163;
- accounting errors 3.4e-16 and 4.8e-14.

**Statistics:**
- dSR -0.0412, Memmel SE .0162, CBB 95% [-.0680, -.0137], rho .99949;
- p one-sided .9984, two-sided .0048;
- DSR (N 61) .8184; PBO .1566.

**4x guard:** 1.5829 vs 1.6994. It is lower by .1165, is printed, and decides nothing.

| for R0-14 | L / mean L_t | net annual (CAGR) | net Sharpe 1x | net Sharpe 4x | vol | max DD |
|---|---|---|---|---|---|---|
| Y-F0 | 1.1828 | 5.654% (5.767%) | 1.8495 | 1.6994 | 3.057% | 2.580% |
| X-10 | 2.0 / 2.0 (fixed; gross 1.6667) | 9.306% (9.606%) | 1.8083 | 1.5829 | 5.146% | 4.326% |

X-10 is now **Y-F**, the levered book offered to the owner, unless Y-1 is accepted. The deployment choice is the owner's
(PM7-3).

## Trials and ledger

- `build-equity/trials.jsonl`: 131 -> **132 lines** (head `15484aa1699daa9a`).
- **1 construction trial**: `25f3b27aae4754ab` (s2_net_sr 1.80829). **N 60 -> 61** of 62.
- 0 admission lines.
- The readers, the bundle, the plan and the lock added 0.

## What R0-11 needs: Y-1 vol-target on Y-F0, the child of X-10

- **Template.** `scripts/specs/v8/y-vol-target.json` on disk is `4e190f54...` = the registered pin (@ `47d6afd9`),
  `i/lf`.
- **Its flags.** The template's root fills are `--risk-model` and `--risk-model-sha256`.
- **Parent.** Y-F0 (`lib-v8ysb-gm.json`, L_P 1.1828). The template text says X-F0; v8y Appendix C (9) settles it as
  Y-F0.
- **Paired reference: X-10's NAV** `build-equity/mega-nav-v8x-theme-erc-L2.0-v8ysb` (`summary.json` `1bc1b6ca`, S2 daily
  `e783eb74`).
- **Open choice for the PM or R0-11** (this report does not decide it). The cycle's summ pairs against the template
  parent's NAV. So either:
  - parent = `x-leverage-L2.0.json`, which resolves to Y-F0 + L 2.0, and summ's dSR is then vs X-10; or
  - parent = Y-F0, and the deciding dSR comes from a bundle vs X-10, with the cycle's dSR printed as "dSR vs Y-F0".
- **P14.** The R-8 store `build-equity/v8-risk-lo3-v10` (manifest `862515d9...`) has `role.manifest_sha256` `e1c67101...`,
  which is Y-F0's lo3 role. On its face the role pin matches. R0-11 must confirm and log this; if it does not match,
  the store is built at 0 trials.
- **Mechanics, written before the run:**
  - gross in [1.0, 2.0] x G_P / L_P widened by .005, which is **[.828796, 1.672591]**;
  - |net| <= .033818, tau <= .338181, p95 <= .507271 (YP-9);
  - before any return, `vol_target.csv`: estimates about scored sessions / 21, decisions before the first estimate,
    clip counts, priced_share near 1.
- **Acceptance:**
  - dSR > 0 vs X-10;
  - AND X-10's rule vs Y-F0 (net annual > .056538 AND Sharpe >= 1.749482);
  - AND mechanics.
- **Budget:** N 61 -> 62 = the cap (the last slot). Y-1 runs by hand (YP-7).
- **Not applicable:** Y-5 was rejected, so YP-15 (the Y-5 composition) does not apply.

## Hard rules

- Nothing was built and no code was changed. The only new repo file is the cell spec.
- Every data run went through `research_cycle.py` or the bounded runner.
- Nothing dated 2024-01-01 or later was opened. Returns were read only after the mechanics passed, and only from TRAIN
  outputs.
- `C:/atx` and `atx-db/` were not touched.
- No push, no worktree prune, no expected hash edited, no subagent.
- `git status` is clean apart from the untracked png, which was left as it is.
- All logs are in `.superpowers/sdd/platform-v8-20260929/integration-log.md`, section "R0-10 X-10 leverage L 2.0 on
  Y-F0".
