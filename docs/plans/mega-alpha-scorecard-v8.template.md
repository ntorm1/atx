# Mega-alpha scorecard v8 and alpha DSL (as of {{PM: date}})

<!--
TEMPLATE (platform v8 REPORT task 3). The PM copies this file to docs/plans/<date>-mega-alpha-scorecard-v8.md after
the cells run and replaces every {{...}} placeholder from the file its alias names (table "Inputs" below). Grammar:
  {{ALIAS.path}}            a value of the JSON file ALIAS (dotted path; list index by number)
  {{SUMM[K].path}}          the row of the nav_summ JSON whose dir is cell K's directory (table "Cells")
  {{PAIRED[K].path}}        cell K's paired test (nav_summ --bundle PARENT K); CAP[K] likewise cell K's capacity file
                            and NAV[K] cell K's NAV summary.json
  {{ALIAS.list[KEY].path}}  the element of the list whose id / key / scenario is KEY (e.g. NAVF.scenarios[S1])
  {{ALIAS.list[i].path}}    one table row per element i;  {{ALIAS.list[*].path}}  every element, comma-joined
  {{TEXT:ALIAS ...}}        text quoted verbatim from a document
  {{PM: ...}} or {{PM}}     a statement the PM writes (a verdict, a ruling, a name), never a computed number
Format as scorecard v6: Sharpe +.3f, dSR +.3f (SE .3f), p .4f, tau .4f, bps .2f, returns +pct1, leverage .3f/.4f.
Optional cells (Rulings E-38, E-45, PM4-8, PM4-10): R-9a..c run only if R-6 is rejected; R-10 and R-11 only if R-6 and
R-1 are accepted; R-12 only if R-6 is accepted (and a candidate passes the screen). A cell whose branch was not taken, or
that could not be formed (PM4-10: the member cap 1/(2T) infeasible), keeps its row with "-" in every value column and
the verdict "undefined (<ruling id>)"; it adds 0 to N. R-9's frontier cells are report only: no dSR, no acceptance.
Hidden-data rule: nothing dated 2024-01-01 or later is opened; no validation statistic is quoted anywhere.
The same inputs render the pitch: python atx-impl/tools/mega_report --config docs/plans/mega-alpha-v8-pitch.config.json
Delete this comment in the filled scorecard.
-->

Integration checkout `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929` at `{{PM: root HEAD}}`. All numbers are TRAIN
2020-2023 (research-window-v2, `[2020-01-01, 2024-01-01)`; nothing dated 2024-01-01 or later was read in v8). Primary cost
scenario **S2** = `modeled-1bn-stale5-v1` impact costs at $1bn NAV x `swap-fin-v1` financing; daily rebalance (cadence
1); 252 sessions/yr; Sharpe on excess returns. Paired tests: studentized circular block bootstrap, block 21, seed
20260929, 4,999 resamples (v8-prereg item 4). Sources: `SUMM` (nav_summ `--protocol v8` over every v8 cell,
`--dsr-ledger LEDGER`), `PAIRED[*]`, `BUNDLE`, `DIAG`, `LEDGER`, the per-cell runner receipts and the ledger
`.superpowers/sdd/platform-v8-20260929/progress.md`. The v7 pitch and scorecard v6 stay the references for 2020-2022.

## Inputs (fill after the cells run)

| alias | path (relative to `C:/atx-wt/pool-2`) | produced by |
|---|---|---|
| `SUMM` | `build-equity/mega-nav-v8-summ.json` | `nav_summ.py --protocol v8 --dsr-ledger build-equity/trials.jsonl --effective-n dirs --psr --json build-equity/mega-nav-v8-summ.json <every cell dir below>` |
| `PAIRED[K]` | `build-equity/mega-nav-v8-paired-<k>.json` (k = b0b, r1 .. r8, r10 .. r12; R-9's frontier cells have none) | `nav_summ.py --protocol v8 --bundle <parent dir> <cell dir> --bundle-json <path>` |
| `BUNDLE` | `build-equity/mega-nav-v8-bundle-b0c-v8f.json` | `nav_summ.py --protocol v8 --bundle <B0c dir> <V8-F dir> --bundle-json <path>` |
| `LEDGER` | `build-equity/trials.jsonl` | the cycle's ledger lines (`--origin`, window id, hash chain) |
| `APPX` | the Appendix A line of `nav_summ.py --protocol v8 --ledger-n build-equity/trials.jsonl` | stdout, pasted verbatim |
| `DIAG` | `build-equity/mega-diagnostics-v8-b0c/diagnostics-v8.json` | `book_diagnostics.py run` on B0c (schema `atx.book-diagnostics/v1`) |
| `CARDS` | `build-equity/mega-cards-v8-<final>/index.json` | `alpha_report_card.py --ic-theta --marginal-ic <K6> --marginal-ic-sha256 <its SHA> --marginal-ic-pool-sha256 <the pool's SHA>` on V8-F (Ruling E-36: K6 bound to the card's role, window, pool and library; marginal IC report-only, rule 8) |
| `ADM` | `build-equity/mega-weights-v8-<final>-ew/admission.json` | `fit_composition_weights.py --report-f-theta` on V8-F |
| `W` | `build-equity/mega-weights-v8-<final>-ew/composition_weights.json` | the same fit |
| `NAVF` | `<V8-F dir>/summary.json` | the final NAV cell (cost and financing scenarios) |
| `NAV[K]` | `<cell K dir>/summary.json` | cell K's NAV run: R-5 reads its and its parent's S3 net Sharpe; R-6 its spo-v3 tripwire and per-book report (`v7.spo_v3_tripwire`, `v7.spo_v3_books`) |
| `CAP[K]` | `<cell K dir>-stress/v7_extras.json` | the capacity curve of R-3 and R-5 (their criteria read 2x and 4x) |
| `LIB`, `RECIPE` | `atx-impl/strategies/alphas/libraries/<final library>.json` and its recipe | `research_cycle.py add-alpha` (A-2) |
| `PREREG` | `.superpowers/sdd/platform-v8-20260929/v8-prereg.md` | root (pins filled) |
| `LIT` | `.superpowers/sdd/platform-v8-20260929/reports/Equity long short alpha v8.md` | the v8 literature review |

## Cells

| key | cell dir (`build-equity/...`) | parent | N after | mechanical criterion (task) |
|---|---|---|---|---|
| B0a | `mega-nav-v8-b0a-lo1-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` | - | 38 | none (re-base) |
| B0b | `mega-nav-v8-b0b-lo3-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` | B0a | 39 | none besides dSR > 0 and mechanics (ruling W0-b) |
| B0c | `mega-nav-v8-b0c` ({{PM: final dir name}}) | winner | 40 | none (protocol correction; baseline by declaration) |
| R-1 | `mega-nav-v8-r1` ({{PM}}) | B0c | 41 | turnover per unit gross not higher |
| R-2 | `mega-nav-v8-r2` ({{PM}}) | last accepted | 42 | turnover not higher |
| R-3 | `mega-nav-v8-r3` ({{PM}}) | last accepted | 43 | net at 2x not lower; turnover lower |
| R-4 | `mega-nav-v8-r4` ({{PM}}) | last accepted | 44 | turnover at least 15% lower |
| R-5 | `mega-nav-v8-r5` ({{PM}}) | last accepted | 45 | net at 4x higher; net at 1x within one SE; S3 not lower |
| R-6 | `mega-nav-v8-r6` ({{PM}}) | last accepted | 46 | cost per traded dollar not higher; tripwire clear; limits_unmet 0 on the scored decisions of the primary book (Ruling E-31: else the run is invalid; E-31a: the run voids itself); traded-book correlation with the aim >= .9 (Ruling E-14; E-14a: the traded book after decision d's trades against the aim at d) |
| R-7 | `mega-nav-v8-r7` ({{PM}}) | last accepted | 47 | turnover not higher (Ruling E-36: the new members' marginal IC is report only, rule 8) |
| R-8 | `mega-nav-v8-r8` ({{PM}}) | last accepted | 48 | realised volatility of the S2 net series inside [.8, 1.2] x sigma_star 5% in each TRAIN year (Ruling E-43: rule 5 governs) |
| R-9a | `mega-nav-v8-r9a` ({{PM}}) | last accepted | 49 | none: report-only frontier cell, theta .03 at 4x NAV, no acceptance (Ruling PM4-8); defined only if R-6 is rejected (Rulings E-38, E-37) |
| R-9b | `mega-nav-v8-r9b` ({{PM}}) | last accepted | 50 | none: report-only frontier cell, theta .04 at 4x NAV, no acceptance (Ruling PM4-8); defined only if R-6 is rejected (Rulings E-38, E-37) |
| R-9c | `mega-nav-v8-r9c` ({{PM}}) | last accepted | 51 | none: report-only frontier cell, theta .05 at 4x NAV, no acceptance (Ruling PM4-8); defined only if R-6 is rejected (Rulings E-38, E-37) |
| R-10 | `mega-nav-v8-r10` ({{PM}}) | last accepted | 49 | planned turnover per unit gross not higher (R-1's criterion, Ruling E-44); defined only if R-6 and R-1 are accepted (Rulings E-38, E-45) |
| R-11 | `mega-nav-v8-r11` ({{PM}}) | last accepted | 50 | planned turnover per unit gross not higher (R-1's criterion, Rulings E-44, PM4-4); defined only if R-6 and R-1 are accepted (Rulings E-38, E-45) |
| R-12 | `mega-nav-v8-r12` ({{PM}}) | last accepted | 51 | book turnover not higher (Ruling PM4-9); defined only if R-6 is accepted (Ruling E-38) |

V8-F = the last accepted cell: **{{PM: key of V8-F}}**.

## 1. Headline

| | cell | S2 net SR | gross SR | gross lev (all rows) | DSR cell count (N) / effective N / Lo | status |
|---|---|---|---|---|---|---|
| **Final cell (V8-F)** | `{{PM: V8-F dir}}` | **{{SUMM[V8-F].net_sharpe}}** | {{SUMM[V8-F].gross_sharpe}} | {{SUMM[V8-F].mean_gross_leverage_all_rows}} | {{SUMM[V8-F].deflated_ledger.dsr}} (N {{SUMM[V8-F].deflated_ledger.n}}) / {{SUMM[V8-F].deflated_effective_n.dsr}} / {{SUMM[V8-F].deflated_lo_null.dsr}} | {{PM: freeze gate MET / UNMET}} |
| Baseline (B0c) | `{{PM: B0c dir}}` | **{{SUMM[B0c].net_sharpe}}** | {{SUMM[B0c].gross_sharpe}} | {{SUMM[B0c].mean_gross_leverage_all_rows}} | {{SUMM[B0c].deflated_ledger.dsr}} | baseline by declaration |
| v7.1 book re-based (B0a) | `mega-nav-v8-b0a-...` | {{SUMM[B0a].net_sharpe}} | {{SUMM[B0a].gross_sharpe}} | {{SUMM[B0a].mean_gross_leverage_all_rows}} | {{SUMM[B0a].deflated_ledger.dsr}} | re-base (the v7.1 book on 2020-2023) |

**{{PM: one sentence: is the pre-registered freeze gate met.}}** Cumulative paired test V8-F vs B0c: dSR
{{BUNDLE.paired.dsr}}, Memmel SE {{BUNDLE.paired.memmel_se}} (t {{BUNDLE.paired.t}}), CBB 95% {{BUNDLE.paired.cbb_ci95}},
studentized bootstrap p one-sided {{BUNDLE.paired.lw.p_one_sided}} (two-sided {{BUNDLE.paired.lw.p_value}}); nav_summ
verdict {{BUNDLE.verdict.pass}}. Year net returns 2020 {{SUMM[V8-F].year_table.0.net_return}} / 2021
{{SUMM[V8-F].year_table.1.net_return}} / 2022 {{SUMM[V8-F].year_table.2.net_return}} / 2023
{{SUMM[V8-F].year_table.3.net_return}}. Planning value: live net Sharpe .7 to 1.0 [est] against the TRAIN figure
{{SUMM[V8-F].net_sharpe}}.

### Freeze gate (v8-prereg item 9, declared before any read)

| condition | value | threshold | result |
|---|---|---|---|
| S2 net Sharpe, V8-F | {{SUMM[V8-F].net_sharpe}} | >= 1.00 | {{PM}} |
| Mechanics: gross leverage, mean over all rows | {{SUMM[V8-F].mean_gross_leverage_all_rows}} | [0.90, 1.05] | {{PM}} |
| Mechanics: net leverage, mean over all rows | {{SUMM[V8-F].mean_net_leverage_all_rows}} | abs <= 0.02 | {{PM}} |
| Mechanics: tau mean / p95 | {{SUMM[V8-F].tau_gmv_mean}} / {{SUMM[V8-F].tau_gmv_p95}} | <= 0.20 / 0.30 | {{PM}} |
| Cumulative paired dSR, V8-F vs B0c | {{BUNDLE.paired.dsr}} | > 0 | {{PM}} |
| Bootstrap p, one-sided | {{BUNDLE.paired.lw.p_one_sided}} | < .10 | {{PM}} |
| Deflated Sharpe, cell count (N = ledger count, OD-4) | {{SUMM[V8-F].deflated_ledger.dsr}} | >= .95 | {{PM}} |
| **Freeze gate** | | all pass | **{{PM: MET / UNMET}}** |

Beside the gate (gate nothing): effective-N DSR {{SUMM[V8-F].deflated_effective_n.dsr}}, PBO {{PM: nav_summ --pbo
json}}, PSR(0) {{SUMM[V8-F].psr.benchmarks.0.psr}}, MinTRL {{SUMM[V8-F].psr.benchmarks.0.min_trl_years}} years.
{{PM: if the gate is unmet, say so and name OD-3 (history extension) as the lever (plan task V8-F step 3).}}

## 2. TRAIN cells (S2 primary)

### 2a. The v8 cells

Acceptance (v8-prereg item 5) = paired S2 net dSR > 0 against the parent AND mechanics AND the mechanical criterion
named in the task; rejected cells are not retried. dSR and its SE come from `PAIRED[K]`; everything else from `SUMM[K]`.

| # | cell | lever | parent | N after | S2 net | gross SR | vol | tau mean/p95 | cost bps/$ | gross all rows | net all rows | dSR vs parent (SE) | p one-sided | mechanical criterion | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | B0a | v7.1 on lo1, 2020-2023 | - | 38 | {{SUMM[B0a].net_sharpe}} | {{SUMM[B0a].gross_sharpe}} | {{PM: NAVF-style ann_vol}} | {{SUMM[B0a].tau_gmv_mean}}/{{SUMM[B0a].tau_gmv_p95}} | {{SUMM[B0a].cost_bps_traded}} | {{SUMM[B0a].mean_gross_leverage_all_rows}} | {{SUMM[B0a].mean_net_leverage_all_rows}} | - | - | none (re-base) | {{PM}} |
| 2 | B0b | v7.1 on lo3 | B0a | 39 | {{SUMM[B0b].net_sharpe}} | {{SUMM[B0b].gross_sharpe}} | {{PM}} | {{SUMM[B0b].tau_gmv_mean}}/{{SUMM[B0b].tau_gmv_p95}} | {{SUMM[B0b].cost_bps_traded}} | {{SUMM[B0b].mean_gross_leverage_all_rows}} | {{SUMM[B0b].mean_net_leverage_all_rows}} | {{PAIRED[B0b].paired.dsr}} ({{PAIRED[B0b].paired.memmel_se}}) | {{PAIRED[B0b].paired.lw.p_one_sided}} | none (W0-b) | {{PM}} |
| 3 | B0c | winner + delisting returns + warm start 60 | winner | 40 | {{SUMM[B0c].net_sharpe}} | {{SUMM[B0c].gross_sharpe}} | {{PM}} | {{SUMM[B0c].tau_gmv_mean}}/{{SUMM[B0c].tau_gmv_p95}} | {{SUMM[B0c].cost_bps_traded}} | {{SUMM[B0c].mean_gross_leverage_all_rows}} | {{SUMM[B0c].mean_net_leverage_all_rows}} | - | - | baseline by declaration | baseline |
| 4 | R-1 | composition v8 | {{PM}} | 41 | {{SUMM[R-1].net_sharpe}} | {{SUMM[R-1].gross_sharpe}} | {{PM}} | {{SUMM[R-1].tau_gmv_mean}}/{{SUMM[R-1].tau_gmv_p95}} | {{SUMM[R-1].cost_bps_traded}} | {{SUMM[R-1].mean_gross_leverage_all_rows}} | {{SUMM[R-1].mean_net_leverage_all_rows}} | {{PAIRED[R-1].paired.dsr}} ({{PAIRED[R-1].paired.memmel_se}}) | {{PAIRED[R-1].paired.lw.p_one_sided}} | turnover per unit gross not higher: {{PM: tau/gross cell vs parent}} | {{PM}} |
| 5 | R-2 | library v8.0 | {{PM}} | 42 | {{SUMM[R-2].net_sharpe}} | {{SUMM[R-2].gross_sharpe}} | {{PM}} | {{SUMM[R-2].tau_gmv_mean}}/{{SUMM[R-2].tau_gmv_p95}} | {{SUMM[R-2].cost_bps_traded}} | {{SUMM[R-2].mean_gross_leverage_all_rows}} | {{SUMM[R-2].mean_net_leverage_all_rows}} | {{PAIRED[R-2].paired.dsr}} ({{PAIRED[R-2].paired.memmel_se}}) | {{PAIRED[R-2].paired.lw.p_one_sided}} | turnover not higher: {{PM}} | {{PM}} |
| 6 | R-3 | persistence gain | {{PM}} | 43 | {{SUMM[R-3].net_sharpe}} | {{SUMM[R-3].gross_sharpe}} | {{PM}} | {{SUMM[R-3].tau_gmv_mean}}/{{SUMM[R-3].tau_gmv_p95}} | {{SUMM[R-3].cost_bps_traded}} | {{SUMM[R-3].mean_gross_leverage_all_rows}} | {{SUMM[R-3].mean_net_leverage_all_rows}} | {{PAIRED[R-3].paired.dsr}} ({{PAIRED[R-3].paired.memmel_se}}) | {{PAIRED[R-3].paired.lw.p_one_sided}} | net at 2x not lower ({{CAP[R-3]}} vs parent); turnover lower: {{PM}} | {{PM}} |
| 7 | R-4 | rank hysteresis | {{PM}} | 44 | {{SUMM[R-4].net_sharpe}} | {{SUMM[R-4].gross_sharpe}} | {{PM}} | {{SUMM[R-4].tau_gmv_mean}}/{{SUMM[R-4].tau_gmv_p95}} | {{SUMM[R-4].cost_bps_traded}} | {{SUMM[R-4].mean_gross_leverage_all_rows}} | {{SUMM[R-4].mean_net_leverage_all_rows}} | {{PAIRED[R-4].paired.dsr}} ({{PAIRED[R-4].paired.memmel_se}}) | {{PAIRED[R-4].paired.lw.p_one_sided}} | turnover at least 15% lower: {{PM}} | {{PM}} |
| 8 | R-5 | ADV holding cap | {{PM}} | 45 | {{SUMM[R-5].net_sharpe}} | {{SUMM[R-5].gross_sharpe}} | {{PM}} | {{SUMM[R-5].tau_gmv_mean}}/{{SUMM[R-5].tau_gmv_p95}} | {{SUMM[R-5].cost_bps_traded}} | {{SUMM[R-5].mean_gross_leverage_all_rows}} | {{SUMM[R-5].mean_net_leverage_all_rows}} | {{PAIRED[R-5].paired.dsr}} ({{PAIRED[R-5].paired.memmel_se}}) | {{PAIRED[R-5].paired.lw.p_one_sided}} | net at 4x higher ({{CAP[R-5]}}); net at 1x within one SE; S3 not lower ({{NAV[R-5].scenarios[S3].net_sharpe}} vs parent {{NAV[R-4].scenarios[S3].net_sharpe}}): {{PM}} | {{PM}} |
| 9 | R-6 | target tracking spo-v3 | {{PM}} | 46 | {{SUMM[R-6].net_sharpe}} | {{SUMM[R-6].gross_sharpe}} | {{PM}} | {{SUMM[R-6].tau_gmv_mean}}/{{SUMM[R-6].tau_gmv_p95}} | {{SUMM[R-6].cost_bps_traded}} | {{SUMM[R-6].mean_gross_leverage_all_rows}} | {{SUMM[R-6].mean_net_leverage_all_rows}} | {{PAIRED[R-6].paired.dsr}} ({{PAIRED[R-6].paired.memmel_se}}) | {{PAIRED[R-6].paired.lw.p_one_sided}} | cost per traded dollar not higher ({{SUMM[R-6].cost_bps_traded}} vs parent); tripwire clear ({{NAV[R-6].v7.spo_v3_tripwire.status}}); limits_unmet 0 on the scored decisions of the primary book, E-31 ({{NAV[R-6].v7.spo_v3_books.modeled-1bn-stale5-v1+swap-fin-v1.limits_unmet}}); traded-book aim correlation (after decision d's trades, aim at d) >= .9, E-14 / E-14a ({{NAV[R-6].v7.spo_v3_books.modeled-1bn-stale5-v1+swap-fin-v1.aim_correlation_traded_after.mean}}): {{PM}} | {{PM}} |
| 10 | R-7 | library v8.1 | {{PM}} | 47 | {{SUMM[R-7].net_sharpe}} | {{SUMM[R-7].gross_sharpe}} | {{PM}} | {{SUMM[R-7].tau_gmv_mean}}/{{SUMM[R-7].tau_gmv_p95}} | {{SUMM[R-7].cost_bps_traded}} | {{SUMM[R-7].mean_gross_leverage_all_rows}} | {{SUMM[R-7].mean_net_leverage_all_rows}} | {{PAIRED[R-7].paired.dsr}} ({{PAIRED[R-7].paired.memmel_se}}) | {{PAIRED[R-7].paired.lw.p_one_sided}} | turnover not higher ({{SUMM[R-7].tau_gmv_mean}} vs parent): {{PM}}; marginal IC of the new members report only, gates nothing (rule 8, Ruling E-36; `CARDS`) | {{PM}} |
| 11 | R-8 | ex-ante risk target | {{PM}} | 48 | {{SUMM[R-8].net_sharpe}} | {{SUMM[R-8].gross_sharpe}} | {{PM}} | {{SUMM[R-8].tau_gmv_mean}}/{{SUMM[R-8].tau_gmv_p95}} | {{SUMM[R-8].cost_bps_traded}} | {{SUMM[R-8].mean_gross_leverage_all_rows}} | {{SUMM[R-8].mean_net_leverage_all_rows}} | {{PAIRED[R-8].paired.dsr}} ({{PAIRED[R-8].paired.memmel_se}}) | {{PAIRED[R-8].paired.lw.p_one_sided}} | realised volatility inside [.04, .06] = [.8, 1.2] x 5% in each TRAIN year, E-43 ({{SUMM[R-8].year_table.0.ann_vol}} / {{SUMM[R-8].year_table.1.ann_vol}} / {{SUMM[R-8].year_table.2.ann_vol}} / {{SUMM[R-8].year_table.3.ann_vol}}): {{PM}} | {{PM}} |
| 12 | R-9a | frontier, theta .03 at 4x NAV | {{PM}} | 49 | {{SUMM[R-9a].net_sharpe}} | {{SUMM[R-9a].gross_sharpe}} | {{PM}} | {{SUMM[R-9a].tau_gmv_mean}}/{{SUMM[R-9a].tau_gmv_p95}} | {{SUMM[R-9a].cost_bps_traded}} | {{SUMM[R-9a].mean_gross_leverage_all_rows}} | {{SUMM[R-9a].mean_net_leverage_all_rows}} | - | - | none: report only, no acceptance (Ruling PM4-8); defined only if R-6 is rejected (E-38, E-37) | {{PM: reported / undefined (E-38, E-37)}} |
| 13 | R-9b | frontier, theta .04 at 4x NAV | {{PM}} | 50 | {{SUMM[R-9b].net_sharpe}} | {{SUMM[R-9b].gross_sharpe}} | {{PM}} | {{SUMM[R-9b].tau_gmv_mean}}/{{SUMM[R-9b].tau_gmv_p95}} | {{SUMM[R-9b].cost_bps_traded}} | {{SUMM[R-9b].mean_gross_leverage_all_rows}} | {{SUMM[R-9b].mean_net_leverage_all_rows}} | - | - | none: report only, no acceptance (Ruling PM4-8); defined only if R-6 is rejected (E-38, E-37) | {{PM: reported / undefined (E-38, E-37)}} |
| 14 | R-9c | frontier, theta .05 at 4x NAV | {{PM}} | 51 | {{SUMM[R-9c].net_sharpe}} | {{SUMM[R-9c].gross_sharpe}} | {{PM}} | {{SUMM[R-9c].tau_gmv_mean}}/{{SUMM[R-9c].tau_gmv_p95}} | {{SUMM[R-9c].cost_bps_traded}} | {{SUMM[R-9c].mean_gross_leverage_all_rows}} | {{SUMM[R-9c].mean_net_leverage_all_rows}} | - | - | none: report only, no acceptance (Ruling PM4-8); defined only if R-6 is rejected (E-38, E-37) | {{PM: reported / undefined (E-38, E-37)}} |
| 15 | R-10 | composition ic-shrink-v1 | {{PM}} | 49 | {{SUMM[R-10].net_sharpe}} | {{SUMM[R-10].gross_sharpe}} | {{PM}} | {{SUMM[R-10].tau_gmv_mean}}/{{SUMM[R-10].tau_gmv_p95}} | {{SUMM[R-10].cost_bps_traded}} | {{SUMM[R-10].mean_gross_leverage_all_rows}} | {{SUMM[R-10].mean_net_leverage_all_rows}} | {{PAIRED[R-10].paired.dsr}} ({{PAIRED[R-10].paired.memmel_se}}) | {{PAIRED[R-10].paired.lw.p_one_sided}} | planned turnover per unit gross not higher, R-1's criterion, E-44: {{PM: tau/gross cell vs parent}}; defined only if R-6 and R-1 are accepted (E-38, E-45) | {{PM: verdict / undefined (E-38, E-45) / undefined (PM4-10)}} |
| 16 | R-11 | composition theme-resid-v1 | {{PM}} | 50 | {{SUMM[R-11].net_sharpe}} | {{SUMM[R-11].gross_sharpe}} | {{PM}} | {{SUMM[R-11].tau_gmv_mean}}/{{SUMM[R-11].tau_gmv_p95}} | {{SUMM[R-11].cost_bps_traded}} | {{SUMM[R-11].mean_gross_leverage_all_rows}} | {{SUMM[R-11].mean_net_leverage_all_rows}} | {{PAIRED[R-11].paired.dsr}} ({{PAIRED[R-11].paired.memmel_se}}) | {{PAIRED[R-11].paired.lw.p_one_sided}} | planned turnover per unit gross not higher, R-1's criterion, E-44 / PM4-4: {{PM: tau/gross cell vs parent}}; defined only if R-6 and R-1 are accepted (E-38, E-45) | {{PM: verdict / undefined (E-38, E-45)}} |
| 17 | R-12 | library v8.2 (add-alpha) | {{PM}} | 51 | {{SUMM[R-12].net_sharpe}} | {{SUMM[R-12].gross_sharpe}} | {{PM}} | {{SUMM[R-12].tau_gmv_mean}}/{{SUMM[R-12].tau_gmv_p95}} | {{SUMM[R-12].cost_bps_traded}} | {{SUMM[R-12].mean_gross_leverage_all_rows}} | {{SUMM[R-12].mean_net_leverage_all_rows}} | {{PAIRED[R-12].paired.dsr}} ({{PAIRED[R-12].paired.memmel_se}}) | {{PAIRED[R-12].paired.lw.p_one_sided}} | book turnover not higher ({{SUMM[R-12].tau_gmv_mean}} vs parent), PM4-9: {{PM}}; marginal IC is the entry screen, gates nothing (rule 8); defined only if R-6 is accepted (E-38) | {{PM: verdict / undefined (E-38)}} |

Not trials (identity checks and window re-runs of ledgered cells, v8-prereg item 2): {{PM: list, each with its identity
result}}. Invalid cells excluded by the defect rule (item 7): {{PM: none / list}}.

### 2b. Year tables (TRAIN 2020-2023)

Per cell and calendar year of the return rows (`SUMM[K].year_table`): net Sharpe / compounded net return. 2023 was read
twice at book level before it entered TRAIN (OD-1): a result that rests on 2023 alone is flagged.

| cell | 2020 | 2021 | 2022 | 2023 | TRAIN |
|---|---|---|---|---|---|
| B0c | {{SUMM[B0c].year_table.0.net_sharpe}} / {{SUMM[B0c].year_table.0.net_return}} | {{SUMM[B0c].year_table.1.net_sharpe}} / {{SUMM[B0c].year_table.1.net_return}} | {{SUMM[B0c].year_table.2.net_sharpe}} / {{SUMM[B0c].year_table.2.net_return}} | {{SUMM[B0c].year_table.3.net_sharpe}} / {{SUMM[B0c].year_table.3.net_return}} | {{SUMM[B0c].net_sharpe}} |
| {{PM: one row per cell, B0a .. R-12 (every cell that is not undefined), same columns}} | | | | | |
| **V8-F** | {{SUMM[V8-F].year_table.0.net_sharpe}} / {{SUMM[V8-F].year_table.0.net_return}} | {{SUMM[V8-F].year_table.1.net_sharpe}} / {{SUMM[V8-F].year_table.1.net_return}} | {{SUMM[V8-F].year_table.2.net_sharpe}} / {{SUMM[V8-F].year_table.2.net_return}} | {{SUMM[V8-F].year_table.3.net_sharpe}} / {{SUMM[V8-F].year_table.3.net_return}} | {{SUMM[V8-F].net_sharpe}} |

V8-F year table in full (return rows, net Sharpe, return, volatility, turnover, cost per traded dollar):

| year | return rows | net Sharpe | net return | volatility | tau mean | cost bps/$ |
|---|---|---|---|---|---|---|
| 2020 | {{SUMM[V8-F].year_table.0.return_rows}} | {{SUMM[V8-F].year_table.0.net_sharpe}} | {{SUMM[V8-F].year_table.0.net_return}} | {{SUMM[V8-F].year_table.0.ann_vol}} | {{SUMM[V8-F].year_table.0.tau_gmv_mean}} | {{SUMM[V8-F].year_table.0.cost_bps_traded}} |
| 2021 | {{SUMM[V8-F].year_table.1.return_rows}} | {{SUMM[V8-F].year_table.1.net_sharpe}} | {{SUMM[V8-F].year_table.1.net_return}} | {{SUMM[V8-F].year_table.1.ann_vol}} | {{SUMM[V8-F].year_table.1.tau_gmv_mean}} | {{SUMM[V8-F].year_table.1.cost_bps_traded}} |
| 2022 | {{SUMM[V8-F].year_table.2.return_rows}} | {{SUMM[V8-F].year_table.2.net_sharpe}} | {{SUMM[V8-F].year_table.2.net_return}} | {{SUMM[V8-F].year_table.2.ann_vol}} | {{SUMM[V8-F].year_table.2.tau_gmv_mean}} | {{SUMM[V8-F].year_table.2.cost_bps_traded}} |
| 2023 | {{SUMM[V8-F].year_table.3.return_rows}} | {{SUMM[V8-F].year_table.3.net_sharpe}} | {{SUMM[V8-F].year_table.3.net_return}} | {{SUMM[V8-F].year_table.3.ann_vol}} | {{SUMM[V8-F].year_table.3.tau_gmv_mean}} | {{SUMM[V8-F].year_table.3.cost_bps_traded}} |

Cumulative dSR V8-F vs B0c by year (`BUNDLE.years`): 2020 {{BUNDLE.years.0.dsr}} / 2021 {{BUNDLE.years.1.dsr}} / 2022
{{BUNDLE.years.2.dsr}} / 2023 {{BUNDLE.years.3.dsr}}.

## 3. Cost and financing stresses (net SR by scenario)

Rows: V8-F, B0c, B0a. Every scenario prices the same construction (`NAVF.scenarios[]`, same for the other cells).

| cell | S1 linear 6 bps | **S2 modeled $1bn** | S2 x engine-tiers | S2 x flat-300 | S3 terminal-adverse (K = 1) | S2-FEE (G-3a, descriptive) |
|---|---|---|---|---|---|---|
| **V8-F** | {{NAVF.scenarios[S1].net_sharpe}} | **{{SUMM[V8-F].net_sharpe}}** | {{NAVF.scenarios[tiers].net_sharpe}} | {{NAVF.scenarios[flat300].net_sharpe}} | {{NAVF.scenarios[S3].net_sharpe}} | {{DIAG.diagnostics.G-3a.result.restated_net_sharpe}} (on B0c) |
| B0c | {{PM}} | **{{SUMM[B0c].net_sharpe}}** | {{PM}} | {{PM}} | {{PM}} | {{DIAG.diagnostics.G-3a.result.restated_net_sharpe}} |
| B0a | {{PM}} | **{{SUMM[B0a].net_sharpe}}** | {{PM}} | {{PM}} | {{PM}} | - |

Capacity (R-5 criterion, `CAP[R-5]`): net Sharpe at .5 / 1 / 2 / 4 / 8 x $1bn {{CAP[R-5].capacity[*].net_sharpe}}.

## 4. How the alphas become the book (final cell)

{{PM: the v6 section 4 walk-through, updated for v8: library (`LIB`, sha), universe (role train-2020-2023-lo1 or lo3,
manifest sha from PREREG pins), admission `v4-prior-v1` (`ADM.counts`), composition (the rule of R-1 if accepted, else
ew-theme-v1; `W` sha), combined signal, desired target, construction (the rules of R-3/R-4/R-5/R-6 as accepted), execution
and costs (`NAVF.scenarios[S2]`).}}

## 5. Alpha table (final library, {{PM: number of ADM.candidates}} candidates)

Status, HAC t and tau are the TRAIN admission statistics (`ADM.candidates[]`); weights from `W`. The traded-horizon
columns are **report only, gates nothing** (v8-prereg item 8): ic_theta and marginal IC21 from `CARDS.candidates[]`,
f_theta and its HAC t from `ADM.candidates[]`.

| id | theme | tier | raw dir | change vs v7.1 | status | HAC t | tau | **w** | ic_theta | f_theta (HAC t) | marginal IC21 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| {{ADM.candidates[i].id}} | {{RECIPE.members[i].theme}} | {{RECIPE.members[i].tier}} | {{RECIPE.members[i].prior_sign}} | {{PM}} | {{ADM.candidates[i].status}} | {{ADM.candidates[i].hac_t}} | {{ADM.candidates[i].tau}} | **{{W.weights[i]}}** | {{CARDS.candidates[i].ic_theta}} | {{ADM.candidates[i].f_theta}} ({{ADM.candidates[i].f_theta_hac_t}}) | {{CARDS.candidates[i].marginal_ic21}} |

Theme mass on the final role: {{PM: theme x members x weight, as v6}}.

## 6. Alpha DSL strings (verbatim from `LIB`)

{{PM: one subsection per theme, each member with its status, weight, citation and DSL in a code block (v6 format).}}

## 7. Diagnostics G-1..G-3 (zero trials, descriptive; `DIAG`, run on B0c)

Declaration: {{DIAG.declaration}}. Window {{DIAG.window_id}}; tool sha256 {{DIAG.tool.script_sha256}}. None gates,
selects or re-weights anything (v8-prereg item 8). A skipped diagnostic is listed with its reason.

| id | question | status | headline figures or reason |
|---|---|---|---|
| G-1a | {{DIAG.diagnostics.G-1a.question}} | {{DIAG.diagnostics.G-1a.status}} | weighted ic_theta {{DIAG.diagnostics.G-1a.result.weighted_ic_theta}}; negative at theta {{DIAG.diagnostics.G-1a.result.negative_at_theta}}; unscored {{DIAG.diagnostics.G-1a.result.unscored}} |
| G-1b | {{DIAG.diagnostics.G-1b.question}} | {{DIAG.diagnostics.G-1b.status}} | combined turnover model / book {{DIAG.diagnostics.G-1b.result.combined_turnover}}; fast members {{DIAG.diagnostics.G-1b.result.fast.ids}} own share {{DIAG.diagnostics.G-1b.result.fast.own_share}} |
| G-1c | {{DIAG.diagnostics.G-1c.question}} | {{DIAG.diagnostics.G-1c.status}} | netting ratio themes model / book {{DIAG.diagnostics.G-1c.result.themes_model.ratio}} / {{DIAG.diagnostics.G-1c.result.themes_book.ratio}} |
| G-2a | {{DIAG.diagnostics.G-2a.question}} | {{DIAG.diagnostics.G-2a.status}} | variance share market / industry / style / specific {{DIAG.diagnostics.G-2a.result.mean_share}} |
| G-2b | {{DIAG.diagnostics.G-2b.question}} | {{DIAG.diagnostics.G-2b.status}} | weighted IC by vol / ADV tercile, top 1,000 {{DIAG.diagnostics.G-2b.result.weighted_mean_ic}} |
| G-2c | {{DIAG.diagnostics.G-2c.question}} | {{DIAG.diagnostics.G-2c.status}} | held / ADV p95, max, share above Q {{DIAG.diagnostics.G-2c.result.held}}; aim / ADV {{DIAG.diagnostics.G-2c.result.aim}} |
| G-3a | {{DIAG.diagnostics.G-3a.question}} | {{DIAG.diagnostics.G-3a.status}} | S2 net {{DIAG.diagnostics.G-3a.result.net_sharpe}} -> S2-FEE {{DIAG.diagnostics.G-3a.result.restated_net_sharpe}} (delta {{DIAG.diagnostics.G-3a.result.delta}}) |
| G-3b | {{DIAG.diagnostics.G-3b.question}} | {{DIAG.diagnostics.G-3b.status}} | IC retained after price-risk-v1 {{DIAG.diagnostics.G-3b.result.members[*].retained}} |
| G-3c | {{DIAG.diagnostics.G-3c.question}} | {{DIAG.diagnostics.G-3c.status}} | net Sharpe delta at delay 1 / 2 / 3 {{DIAG.diagnostics.G-3c.result.delays[*].delta}} |
| G-3d | {{DIAG.diagnostics.G-3d.question}} | {{DIAG.diagnostics.G-3d.status}} | adjusted Rand vs themes {{DIAG.diagnostics.G-3d.result.adjusted_rand_index}}; effective bets themes {{DIAG.diagnostics.G-3d.result.effective_bets_themes}} |

## 8. Trial accounting (Appendix A)

```
{{APPX}}   <- the Appendix A line, then insert " plus 8 re-screens" after "admission trials this sprint <k>" (ruling R2-e)
Trial accounting (TRAIN 2020-2023 only; hidden 2024+ unread in this sprint; no validation statistic read):
  before v8: N 37 (ledger through v7.1); validation reads before v8: 2 (2023-2024); 2025+ never read.
  v8 re-base: B0a, B0b, B0c (N 38-40); window re-runs of ledgered cells add 0 ({{PM: count}}).
  v8 construction cells: {{PM: R-1 .. R-12 as run, each with its verdict; an undefined cell with its ruling, adding 0}} -> N {{SUMM[V8-F].deflated_ledger.n}}.
  admission: {{PM: 7 (library v8.0) + <k> (library v8.1)}} plus 8 `_f49` re-screens at 0 trials (ruling R2-e).
  budget: N <= 51, admission <= 15 (plan 12.1): {{PM: within / exceeded}}.
  Not trials: identity checks, window re-runs (item 2), invalid cells (item 7): {{PM: list}}.
  DSR (V8-F, N = {{SUMM[V8-F].deflated_ledger.n}}, T {{SUMM[V8-F].net_moments.sessions}}):
    OD-4 variance (cells scored on research-window-v2): DSR {{SUMM[V8-F].deflated_ledger.dsr}}, SR0 {{SUMM[V8-F].deflated_ledger.sr0_annual}} ann
    legacy variance (reported, gates nothing): DSR {{SUMM[V8-F].deflated_ledger.legacy_dsr}}, SR0 {{SUMM[V8-F].deflated_ledger.legacy_sr0_annual}} ann
```

## 9. OD-1 disclosure (v8-prereg item 1)

{{TEXT:PREREG item 1, verbatim}}

- Window research-window-v2: TRAIN `[2020-01-01, 2024-01-01)`; sealed from 2024-01-01 (every tool refuses a session on or
  after it).
- 2023 enters TRAIN partly selected: TRAIN statistics on 2023 flatter the incumbent by up to about .08 in a paired
  difference [est].
- 2024 stays hidden but is not pristine. Only 2025 and later has never been read.
- The standing rule never to read a per-candidate validation statistic no longer covers 2023. It covers 2024 and later.

## 10. Contradictions with the v6 and v7 literature

Verbatim from `LIT`, section "Where the new notes contradict or update v6 and v7" (literature figures, not measurements
on the book). "Contradicts" = the earlier recommendation is withdrawn; "updates" = its number or scope changes;
"confirms" = new evidence strengthens it.

| Earlier position | New evidence in the v8 notes | Verdict |
|---|---|---|
| {{TEXT:LIT table rows, one per row}} | | |

{{PM: for each "contradicts" row, one line on what v8 did about it (built, withdrawn, deferred), with the cell or
ruling id.}}
