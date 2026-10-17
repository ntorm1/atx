# P9-L independent derivation (CELLS-2)

Verdict: **ACCEPTED**. All six finite mechanics values pass the registered,
unscaled limits. Under PRE-5 this protocol cell is adopted as the new parent;
the paired result does not enter that decision.

Derived by a fresh GPT-6 Astra agent on 2026-10-17 in
`C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`, under
`r14-prompts/deriv-p9l-prompt-2.md` and its last-wins RESUME 15 amendment.
The separately mandated commit trailer is literal attribution text, not the
identity of the model that performed this derivation.

All paths below are relative to `C:/atx-wt/pool-2` unless prefixed ROOT-SDD;
ROOT-SDD is `.superpowers/sdd/platform-p9-20261003/`.

## Rule and deciding evidence

The rule comes from ROOT-SDD `p9-prereg.md` B.1 (PRE-5), E G-B9 and
`cells/p9l-label-terminal-plan.md` section 2. P9-L uses unscaled limits, with
the two accounting checks added explicitly by the plan. The permitted
`scripts/wave_rules.py:79-101` excerpt includes `MECHANICS["v8-mech"]` at
lines 92-93 and confirms the four exposure/turnover limits.

Source for every value in this table:
`build-equity/p9l-label-terminal-readers/mech-matched.json`.
JSON paths are explicit; the tolerance is from that same file.

| Mechanics row | JSON path | Value | Limit | Result |
|---|---|---:|---:|---|
| gross_all_rows | `$.navs.cell.mean_gross_leverage_all_rows` | 0.9862134133265011 | [0.90, 1.05] | PASS |
| abs_net_all_rows | `abs($.navs.cell.mean_net_leverage_all_rows)` | 0.005339981852022642 | <= 0.02 | PASS |
| tau_mean | `$.navs.cell.tau_gmv_mean` | 0.028134396709054203 | <= 0.20 | PASS |
| tau_p95 | `$.navs.cell.tau_gmv_p95` | 0.033410443831757175 | <= 0.30 | PASS |
| max_return_identity_error | `$.navs.cell.accounting.max_return_identity_error` | 3.67544536472586e-16 | <= 1e-09 | PASS |
| max_cash_book_relative_error | `$.navs.cell.accounting.max_cash_book_relative_error` | 1.1074477435259236e-13 | <= 1e-09 | PASS |

The last two limits are
`$.navs.cell.accounting.tolerance = 1e-09`. The selected scenario is
`$.navs.cell.scenario = "modeled-1bn-stale5-v1+swap-fin-v1"`; the directory is
`$.navs.cell.dir = "build-equity/mega-nav-p9l-label-terminal-gm"`.
The NAV summary independently carries the same accounting values at
`build-equity/mega-nav-p9l-label-terminal-gm/summary.json`,
`$.scenarios[1].accounting_checks.{max_return_identity_error,max_cash_book_relative_error,tolerance}`.
Turnover is taken from the plan's designated mechanics reader, not substituted
with the differently scoped summary turnover fields.

Deciding line: all six rows above PASS; therefore ACCEPTED. Neither zero dSR
nor the bundle's generic freeze-gate verdict can replace PRE-5.

## Calibration and L-prime agreement

Plan section 3 steps 2-4 and the last-wins amendment specify one calibrated
NAV, published in the `-gm` directory. There is no separate calibration NAV
directory and no second NAV is required.

The following values come from
`build-equity/mega-nav-p9l-label-terminal-gm/summary.json`:

| JSON path | Value | Check |
|---|---|---|
| `$.calibration.leverage_requested` | 1.1828 | Equals plan section 3 starting L |
| `$.calibration.target` | 0.9862134133265011 | Equals plan section 1 G_parent |
| `$.calibration.gross_requested` | 0.9862134133265011 | Equals target |
| `$.calibration.gross` | 0.9862134133265011 | Exact same serialized decimal as the mechanics reader's all-rows gross |
| `$.calibration.tolerance` | 0.005 | Equals registered gross-match tolerance |
| `$.calibration.matched` | true | Required match holds |
| `$.calibration.corrected` | false | Plan step 3 therefore retains L-prime 1.1828 |
| `$.calibration.leverage` | 1.1828 | Agrees with derived L-prime |
| `$.calibration.construction_reused` | true | Provenance field present |

The exact same decimal representation of the two gross values establishes
their agreement without rounding. All calibration fields shown in the
existing record are present; `$.calibration.rule` describes the one-process
publication and correction rule.

The plain spec `scripts/specs/p9/templates/label-terminal-gm.json` carries
`$.change.set['nav.leverage'] = "1.1828"` and
`$.change.set['nav.output'] = "build-equity/mega-nav-p9l-label-terminal-gm"`.
It adds no NAV calibration flag. Its description identifies that same L and
the calibration recipe that published the NAV.

ROOT-SDD `cells/logs/p9l-plan-label-terminal-gm.txt`, the saved `# phase nav`
command, carries `--aim-leverage 1.1828` and no `--calibrate-*` flag. Its output
is the published `-gm` directory. This is a saved text artifact, not a command
executed during DERIV. The saved spec SHA-256 agrees with the file hash below.

`build-equity/mega-nav-p9l-label-terminal-gm/recipe.json` has
`$.aim_leverage = 1.1828`. The summary also has
`$.scenarios[1].construction.v5.aim_leverage = 1.1828`.

The actual receipt,
`build-equity/mega-nav-p9l-label-terminal-gm-run/receipt.json`, correctly has
`$.command` flag/value pairs `--aim-leverage 1.1828`,
`--calibrate-gross 0.9862134133265011`, `--calibrate-tolerance 0.005`, and the
same `--output` directory. Its `$.outcome = "completed"`, `$.exit_code = 0`,
and `$.executable_sha256 = 255b58acedf7df82a1caa672ff30a58c317a8f0dccf7dbe7d3b7393b91063565`,
equal to the NAV pin in plan section 1. Its limits are
`$.limits.seconds = 180.0`, `$.limits.max_rss_mib = 1536`, and
`$.limits.min_free_mib = 512`, consistent with the plan's NAV caps.
The actual calibration flags are expected under section 3 step 4 and the
amendment; they are not grounds for a second NAV or a void verdict.

## Paired line, printed only

ROOT-SDD `cells/logs/p9l-rho.txt` was read before the paired bundle. It reports
rho 1.0, stable 1 - rho 0.0, common sessions 1005, and a true degenerate flag.

Existing output `build-equity/p9l-label-terminal-readers/bundle-parent.json`
confirms the following:

| JSON path | Stored value | Printed value |
|---|---|---|
| `$.paired.dsr` | 0.0 | dSR = 0.0 |
| `$.paired.memmel_se` | null | SE = na |
| `$.paired.t` | null | t = na |
| `$.paired.lw.p_one_sided` | null | one-sided p = na |
| `$.paired.lw.p_value` | null | two-sided p = na |
| `$.paired.rho` | 1.0 | rho = 1.0 |
| `$.paired.one_minus_rho` | 0.0 | 1 - rho = 0.0 |
| `$.paired.degenerate` | true | degenerate |
| `$.paired.sessions` | 1005 | common sessions = 1005 |

Since 0.0 < 1e-6, apply C.11/PRE-12's degenerate-pair printing fallback:
**dSR 0.0; SE na; t na; both p na.** P9-L's registered mechanics-only rule
remains decisive. No positive-dSR gate is added to this protocol cell.

The bundle's `$.nav_summ_run.engine_exe_sha256` is
`e2b48c46055c6aefcda0b992f8c0341f7275274793bc30fbb07791f7a58d962c`,
equal to the pinned eval SHA-256 in plan section 1. Its `$.base` and `$.final`
identify the parent `p9-b0g-yf0-nav` and the published cell respectively, and
`$.scenarios.base` and `$.scenarios.final` select the primary S2 scenario.
Its `$.verdict.pass = false` is explicitly a cumulative freeze-gate paired
test, not P9-L's adoption rule.

After deriving the mechanics result, I read the primary scenario summaries
for both `build-equity/mega-nav-p9l-label-terminal-gm/summary.json` and
`build-equity/p9-b0g-yf0-nav/summary.json`. Both have
`$.scenarios[1].ann_mean = 0.05653814276222473`,
`$.scenarios[1].net_sharpe = 1.849482026329859`, and
`$.scenarios[1].ann_vol = 0.030569717335626075`. These are descriptive only.

## File binding and scope

SHA-256 hashes read during DERIV:

| File | SHA-256 |
|---|---|
| `scripts/specs/p9/templates/label-terminal-gm.json` | `de44a668999ab8997c7118afb52efd93551ac2e7df93efe9118edbf719aa9a4d` |
| `build-equity/mega-nav-p9l-label-terminal-gm/summary.json` | `42ddc5cdc202f1988c1106c403f769d322cd79e225744124860cfad134401e66` |
| `build-equity/mega-nav-p9l-label-terminal-gm-run/receipt.json` | `6a7a4ff131f3a0c9791cbd00219942a35b2db9e2776e53d553c50eae1071ea57` |
| `build-equity/p9l-label-terminal-readers/mech-matched.json` | `2ba459ca68cc513f4b896f99a5507cdec946deb34eb501590cd165c9f929fc68` |
| `build-equity/p9l-label-terminal-readers/bundle-parent.json` | `e3b60f29c31c9a7f75139abc24ab8afc556f18eefd7e4c66cdb314469bb20038` |
| `build-equity/p9-b0g-yf0-nav/summary.json` | `826d5ec65329c24c4c0c1b72ee1f57dcdbdb136cf00d4113c6463310e79b8f38` |

The parent summary hash equals the registered reference pin. Only existing
outputs were read; no research program, reader, build, test, calibration,
trial write, or RAM wait was executed. No atx-db path or session data from
2024 onward was opened. Artifact timestamps in the receipt are wall-clock
provenance. A.6 counts a scored P9-L protocol cell as N_c +1 and K_a 0; its
calibration is zero trials. This document performs no ledger update.

This is an independent output derivation, not a claim to have re-audited
unavailable execution history. The root record, handoff, ledger, reviews,
cache contents, executable files, and pre-cell logs were not opened.

## Anchoring disclosure and deviations

Before deriving, I saw the parent assignment and assigned prompt; prereg
headings and requested sections; the plan; mechanics output and permitted
`wave_rules.py:79-101`; then the plain spec, saved planned argv, actual
receipt, rho text, and summary calibration fields. The rho/recipe checks
confirmed the mechanics result before I read the paired bundle and summary
return statistics. No other agent's P9-L verdict, root cell record, handoff,
ledger, previous review, git log, or commit subject was read.

One prereg extraction was too broad: matching references to C.11, C.14 and
G-B9 with context also displayed snippets outside the requested prereg
sections, including historical cells' accepted step values, standing-line
text, and appendix/ruling references. These were all inside `p9-prereg.md`;
no referenced file was opened. Historical ACCEPTED/ACCEPT/HOLDS words and
the plan's alternative verdict words were visible. No prior P9-L result was
exposed. This read-scope deviation is disclosed rather than claiming a
pristine section-only read. Some initial file reads used PowerShell directly
rather than the requested `rtk proxy` wrapper; later raw text reads used it.

No permission refusal occurred. The attempted SHA-256 utility was absent:
`rtk: Failed to resolve 'sha256sum' via PATH, falling back to direct exec: Binary 'sha256sum' not found on PATH`
and `rtk: Failed to execute command: sha256sum: program not found`.
This was a missing executable, not a refused operation; the hashes above
were obtained by read-only PowerShell `Get-FileHash -Algorithm SHA256`.

Open question for the PM: accept the disclosed prereg section-read deviation
for CELLS-2 independence, or commission another fresh derivation. The
independently derived mechanics verdict is ACCEPTED either way; no P9-L
result from another author informed it.
