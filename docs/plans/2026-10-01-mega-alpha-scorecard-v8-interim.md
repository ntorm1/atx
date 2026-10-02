# Mega-alpha scorecard v8 and alpha DSL (as of 2026-10-01; INTERIM at the owner stop)

## Interim status (owner stop, 2026-10-01)

This is an interim scorecard, ordered by the owner's stop of the v8 sprint (Ruling PM5-27). It is rendered from the registered template on the cells that exist; every cell that has not run is marked pending.

- Run: B0a, B0b and B0c, on TRAIN 2020-2023 with S2 as the primary scenario. Each of them is ledgered.
- B0c is the re-based v8 baseline (baseline by declaration, v8-prereg item 6). Its S2 net Sharpe is +1.133 (section 1).
- No improvement over B0c is claimed. No construction cell is accepted and no cumulative test (V8-F against B0c) exists.
- R-1: constructed; mechanics fail (all-rows gross 1.0672, limit [.90, 1.05]); no return read; not ledgered; ruling open.
- The freeze gate (v8-prereg item 9) is not evaluated. V8-F does not exist.
- The deflated Sharpe is not meaningful yet. Its pre-registered cross-trial variance rests on three cells (B0a, B0b, B0c) until the registered re-runs of the ledgered v7 cells on the four-year window are done (Ruling PM5-22). It is printed only where the tool prints it (sections 1 and 8), with this note beside it.
- TRAIN construction cells N 40; admission trials this sprint 0; history reads 0; hidden 2024+ unread in this sprint; validation reads before v8: 2 (the Appendix A block below).
- Cells still to run: the ruling on R-1, R-2..R-8, then R-9a..R-9c or R-10..R-12 (the branch set by R-6), the re-runs of the ledgered v7 cells on the four-year role (PM5-22; they add 0 to N), V8-F.
- The pitch rendered from the interim config (`docs/plans/2026-10-01-mega-alpha-v8-interim-pitch.html`) shows 37 unavailable or refused blocks, each naming its missing input; integration-log.md section "interim report (owner stop)" lists them.

Appendix A block (`nav_summ.py --protocol v8 --ledger-n build-equity/trials.jsonl`, stdout verbatim; R-2 has not run, so no `_f49` re-screen exists and ruling R2-e's " plus 8 re-screens" is not inserted):

```
TRAIN construction cells 40; admission trials this sprint 0; window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history reads 0; 2025+ never read.
```

Integration checkout `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929` at `1f8a0fb4 (render; the interim config commit)`. All numbers are TRAIN
2020-2023 (research-window-v2, `[2020-01-01, 2024-01-01)`; nothing dated 2024-01-01 or later was read in v8). Primary cost
scenario **S2** = `modeled-1bn-stale5-v1` impact costs at $1bn NAV x `swap-fin-v1` financing; daily rebalance (cadence
1); 252 sessions/yr; Sharpe on excess returns. Paired tests: studentized circular block bootstrap, block 21, seed
20260929, 4,999 resamples (v8-prereg item 4). Sources: `SUMM` (nav_summ `--protocol v8` over every v8 cell,
`--dsr-ledger LEDGER`), `PAIRED[*]`, `BUNDLE`, `DIAG`, `LEDGER`, the per-cell runner receipts and the ledger
`.superpowers/sdd/platform-v8-20260929/progress.md`. The v7 pitch and scorecard v6 stay the references for 2020-2022.

## Inputs (interim: what exists at the owner stop)

| alias | path (relative to `C:/atx-wt/pool-2`) | produced by |
|---|---|---|
| `SUMM` | `build-equity/mega-nav-v8-summ-interim.json` (sha256 `d23cb666eb1b2278ff5d1d3c07fbcac79917a06d38f3056525e183dc8708258c`) | bounded `nav_summ.py --protocol v8 --dsr-ledger build-equity/trials.jsonl --effective-n dirs --psr --json build-equity/mega-nav-v8-summ-interim.json` over the B0a, B0b and B0c dirs (no `--ledger`: no line written) |
| `PAIRED[B0b]` | `build-equity/v8-cells-b0b-bundle.json` (sha256 `75c30860398c4d6d10b94dee899b84709f630470a65470f70f146e2db5383191`) | `nav_summ.py --protocol v8 --bundle <B0a dir> <B0b dir> --bundle-json <path>` (cells batch 1a, bounded, no `--ledger`) |
| `PAIRED[R-1 .. R-12]` | pending | R-1: not run (stopped at mechanics; no return read); R-2 .. R-12 not run |
| B0c vs B0b (information only) | `build-equity/v8-cells-b0c-bundle.json` (sha256 `fe849547a537baf6c1a0729e7564409279ee9a51943a17fef891360447b6e70f`) | PM5-23 bundle of cells batch 1b (B0c is the baseline by declaration; it gates nothing) |
| `BUNDLE` | pending (no V8-F) | - |
| `LEDGER` | `build-equity/trials.jsonl` (sha256 `ed3f4139273a085ccdb6c834ac5307bf311231a19136b7807b91d8a0a6b260a1`) | the cycle's ledger lines (41 lines; unchanged by this report) |
| `APPX` | `build-equity/mega-nav-v8-appx-interim-run/stdout.log` (sha256 `72de67766c502232bef22575b3462f45ce374c34a3847c346ed279a8992f6df7`) | bounded `nav_summ.py --protocol v8 --ledger-n build-equity/trials.jsonl` |
| `DIAG` | not produced as one `diagnostics-v8.json` | E-18 ran G-1..G-3 on B0c as eight split runs; no tool verb combines them and they are not merged. Section 7 reads each id from its split file: `build-equity/b0c-diagnostics/diagnostics-v8-g1a-g3d.json` (`de1990b8992f`), `build-equity/b0c-diagnostics/diagnostics-v8-g1b-g1c.json` (`a985adf86049`), `build-equity/b0c-diagnostics/diagnostics-v8-g2a.json` (`e2a9874bd1fd`), `build-equity/b0c-diagnostics/diagnostics-v8-g2b.json` (`86bf47af9615`), `build-equity/b0c-diagnostics/diagnostics-v8-g2c.json` (`cbed04d7219d`), `build-equity/b0c-diagnostics/diagnostics-v8-g3a.json` (`b44ea5d08efe`), `build-equity/b0c-diagnostics/diagnostics-v8-g3b.json` (`19e5e5da96b8`), `build-equity/b0c-diagnostics/diagnostics-v8-g3c.json` (`9f00b5f3afff`) |
| `CARDS`, `ADM`, `W`, `NAVF`, `LIB`, `RECIPE` | pending (no V8-F) | - |
| `NAV[K]` | `build-equity/mega-nav-v8-b0a-lo1-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247/summary.json` (`eeac83664e9a`); `build-equity/mega-nav-v8-b0b-lo3-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247/summary.json` (`82a596c14a12`); `build-equity/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247/summary.json` (`119d4cd0cb47`) | the NAV runs of B0a, B0b, B0c (cost scenarios, whole-window S2 volatility) |
| `CAP[R-3]`, `CAP[R-5]` | pending | - |
| B0c capacity (E-29) | `build-equity/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247/v7_extras.json` (sha256 `eb388cf0e1785737f4d1828a148223739733e9462da3e9eebe75f47c8dfa4153`) | B0c's NAV run with `--capacity-curve` (report only) |
| `PREREG` | `.superpowers/sdd/platform-v8-20260929/v8-prereg.md` | root (pins filled) |
| `LIT` | `.superpowers/sdd/platform-v8-20260929/reports/Equity long short alpha v8.md` | the v8 literature review |

## Cells

| key | cell dir (`build-equity/...`) | parent | N after | mechanical criterion (task) |
|---|---|---|---|---|
| B0a | `mega-nav-v8-b0a-lo1-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` | - | 38 | none (re-base) |
| B0b | `mega-nav-v8-b0b-lo3-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` | B0a | 39 | none besides dSR > 0 and mechanics (ruling W0-b) |
| B0c | `mega-nav-v8-b0c` (mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247) | winner | 40 | none (protocol correction; baseline by declaration) |
| R-1 | `mega-nav-v8-r1` (R-1: constructed; mechanics fail (all-rows gross 1.0672, limit [.90, 1.05]); no return read; not ledgered; ruling open) | B0c | 41 (plan; not ledgered, N stays 40) | turnover per unit gross (executed: tau_gmv_mean / mean_gross_leverage_all_rows, S2) not higher than the parent (Ruling PM5-11) |
| R-2 | `mega-nav-v8-r2` (pending run) | last accepted | 42 | turnover not higher |
| R-3 | `mega-nav-v8-r3` (pending run) | last accepted | 43 | net at 2x not lower; turnover lower |
| R-4 | `mega-nav-v8-r4` (pending run) | last accepted | 44 | turnover at least 15% lower |
| R-5 | `mega-nav-v8-r5` (pending run) | last accepted | 45 | net at 4x higher; net at 1x within one SE; S3 not lower |
| R-6 | `mega-nav-v8-r6` (pending run) | last accepted | 46 | cost per traded dollar not higher; tripwire clear; limits_unmet 0 on the scored decisions of the primary book (Ruling E-31: else the run is invalid; E-31a: the run voids itself); traded-book correlation with the aim >= .9 (Ruling E-14; E-14a: the traded book after decision d's trades against the aim at d) |
| R-7 | `mega-nav-v8-r7` (pending run) | last accepted | 47 | turnover not higher (Ruling E-36: the new members' marginal IC is report only, rule 8) |
| R-8 | `mega-nav-v8-r8` (pending run) | last accepted | 48 | realised volatility of the S2 net series inside [.8, 1.2] x sigma_star 5% in each TRAIN year (Ruling E-43: rule 5 governs) |
| R-9a | `mega-nav-v8-r9a` (pending run) | last accepted | 49 | none: report-only frontier cell, theta .03 at 4x NAV, no acceptance (Ruling PM4-8); defined only if R-6 is rejected (Rulings E-38, E-37) |
| R-9b | `mega-nav-v8-r9b` (pending run) | last accepted | 50 | none: report-only frontier cell, theta .04 at 4x NAV, no acceptance (Ruling PM4-8); defined only if R-6 is rejected (Rulings E-38, E-37) |
| R-9c | `mega-nav-v8-r9c` (pending run) | last accepted | 51 | none: report-only frontier cell, theta .05 at 4x NAV, no acceptance (Ruling PM4-8); defined only if R-6 is rejected (Rulings E-38, E-37) |
| R-10 | `mega-nav-v8-r10` (pending run) | last accepted | 49 | turnover per unit gross (executed: tau_gmv_mean / mean_gross_leverage_all_rows, S2) not higher than the parent (R-1's criterion, Rulings E-44, PM5-11); defined only if R-6 and R-1 are accepted (Rulings E-38, E-45) |
| R-11 | `mega-nav-v8-r11` (pending run) | last accepted | 50 | turnover per unit gross (executed: tau_gmv_mean / mean_gross_leverage_all_rows, S2) not higher than the parent (R-1's criterion, Rulings E-44, PM4-4, PM5-11); defined only if R-6 and R-1 are accepted (Rulings E-38, E-45) |
| R-12 | `mega-nav-v8-r12` (pending run) | last accepted | 51 | book turnover not higher (Ruling PM4-9); defined only if R-6 is accepted (Ruling E-38) |

V8-F = the last accepted cell: **none at the owner stop (no construction cell is accepted; V8-F pending)**.

## 1. Headline

| | cell | S2 net SR | gross SR | gross lev (all rows) | DSR cell count (N) / effective N / Lo | status |
|---|---|---|---|---|---|---|
| **Final cell (V8-F)** | pending (no V8-F) | **pending (no V8-F)** | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | freeze gate not evaluated |
| Baseline (B0c) | `mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` | **+1.133** | +1.552 | 0.9820 | 0.984 (N 40; not meaningful yet: 3-cell variance, PM5-22) / 0.985 / 0.746 | baseline by declaration |
| v7.1 book re-based (B0a) | `mega-nav-v8-b0a-...` | +1.125 | +1.537 | 0.9709 | 0.984 (N 40; not meaningful yet: 3-cell variance, PM5-22) / 0.984 / 0.740 | re-base (the v7.1 book on 2020-2023) |

**The pre-registered freeze gate is not evaluated: V8-F does not exist at the owner stop.** Cumulative paired test V8-F vs B0c: pending (`BUNDLE` not produced). Year net returns of V8-F: pending. Planning value: live net Sharpe .7 to 1.0 [est] against the TRAIN figure of V8-F, pending. The DSR column reads the ledger N and the effective N / Lo null of the three listed dirs; none of them is meaningful yet (PM5-22).

### Freeze gate (v8-prereg item 9, declared before any read)

| condition | value | threshold | result |
|---|---|---|---|
| S2 net Sharpe, V8-F | pending (no V8-F) | >= 1.00 | not evaluated |
| Mechanics: gross leverage, mean over all rows | pending (no V8-F) | [0.90, 1.05] | not evaluated |
| Mechanics: net leverage, mean over all rows | pending (no V8-F) | abs <= 0.02 | not evaluated |
| Mechanics: tau mean / p95 | pending (no V8-F) / pending (no V8-F) | <= 0.20 / 0.30 | not evaluated |
| Cumulative paired dSR, V8-F vs B0c | pending (no V8-F) | > 0 | not evaluated |
| Bootstrap p, one-sided | pending (no V8-F) | < .10 | not evaluated |
| Deflated Sharpe, cell count (N = ledger count, OD-4) | pending (no V8-F) | >= .95 | not evaluated |
| **Freeze gate** | | all pass | **NOT EVALUATED (no V8-F)** |

Beside the gate (gate nothing): pending (no V8-F). The gate is not evaluated, so it is not "unmet": OD-3 is not invoked.

## 2. TRAIN cells (S2 primary)

### 2a. The v8 cells

Acceptance (v8-prereg item 5) = paired S2 net dSR > 0 against the parent AND mechanics AND the mechanical criterion
named in the task; rejected cells are not retried. dSR and its SE come from `PAIRED[K]`; everything else from `SUMM[K]`.
Ruling PM5-11 (declared before any read): the turnover criterion of the composition cells R-1, R-10 and R-11 reads
executed one-way GMV turnover per unit of gross, `tau_gmv_mean / mean_gross_leverage_all_rows` on S2, cell against
parent, "not higher" meaning <=; nav_summ carries no planned-turnover statistic. Where fill caps or blocked orders make
planned and executed turnover disagree in sign against the parent, these cells are judged on the executed number.

| # | cell | lever | parent | N after | S2 net | gross SR | vol | tau mean/p95 | cost bps/$ | gross all rows | net all rows | dSR vs parent (SE) | p one-sided | mechanical criterion | verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | B0a | v7.1 on lo1, 2020-2023 | - | 38 | +1.125 | +1.537 | 3.96% | 0.0367/0.0439 | 13.13 | 0.9709 | +0.0038 | - | - | none (re-base) | re-base ledgered |
| 2 | B0b | v7.1 on lo3 | B0a | 39 | +1.139 | +1.558 | 3.89% | 0.0370/0.0439 | 13.10 | 0.9663 | +0.0035 | +0.014 (0.040) | 0.3820 | none (W0-b) | accepted |
| 3 | B0c | winner + delisting returns + warm start 60 | winner | 40 | +1.133 | +1.552 | 3.90% | 0.0341/0.0390 | 13.13 | 0.9820 | +0.0036 | - | - | baseline by declaration | baseline by declaration |
| 4 | R-1 | composition v8 | B0c | not ledgered (N stays 40) | not read (R-1) | not read (R-1) | not read (R-1) | not read (R-1) | not read (R-1) | not read (R-1) | not read (R-1) | not run | not run | turnover per unit gross (executed: tau_gmv_mean / mean_gross_leverage_all_rows, S2) not higher than the parent, PM5-11: not read | R-1: constructed; mechanics fail (all-rows gross 1.0672, limit [.90, 1.05]); no return read; not ledgered; ruling open |
| 5 | R-2 | library v8.0 | last accepted | 42 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 6 | R-3 | persistence gain | last accepted | 43 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 7 | R-4 | rank hysteresis | last accepted | 44 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 8 | R-5 | ADV holding cap | last accepted | 45 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 9 | R-6 | target tracking spo-v3 | last accepted | 46 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 10 | R-7 | library v8.1 | last accepted | 47 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 11 | R-8 | ex-ante risk target | last accepted | 48 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 12 | R-9a | frontier, theta .03 at 4x NAV | last accepted | 49 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 13 | R-9b | frontier, theta .04 at 4x NAV | last accepted | 50 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 14 | R-9c | frontier, theta .05 at 4x NAV | last accepted | 51 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 15 | R-10 | composition ic-shrink-v1 | last accepted | 49 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 16 | R-11 | composition theme-resid-v1 | last accepted | 50 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |
| 17 | R-12 | library v8.2 (add-alpha) | last accepted | 51 (plan) | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending | pending run |

B0c against B0b, information only (B0c is the baseline by declaration; gates nothing; `build-equity/v8-cells-b0c-bundle.json`, PM5-23): dSR +0.006 (SE 0.017), studentized bootstrap p one-sided 0.3014, two-sided 0.6792. The volatility column is the whole-window annualised S2 volatility of each cell's NAV summary.json (`NAV[K]`).

Not trials (identity checks and window re-runs of ledgered cells, v8-prereg item 2): no window re-run is ledgered (the Appendix A block counts 0 window re-run lines; the PM5-22 re-runs are not run); the identity checks of the integrations and Wave 0 are recorded in integration-log.md and add no trial. Invalid cells excluded by the defect rule (item 7): none (0 defect lines).

### 2b. Year tables (TRAIN 2020-2023)

Per cell and calendar year of the return rows (`SUMM[K].year_table`): net Sharpe / compounded net return. 2023 was read
twice at book level before it entered TRAIN (OD-1): a result that rests on 2023 alone is flagged.

| cell | 2020 | 2021 | 2022 | 2023 | TRAIN |
|---|---|---|---|---|---|
| B0c | +0.319 / +1.4% | +2.352 / +7.7% | +1.936 / +8.6% | +0.118 / +0.3% | +1.133 |
| B0a | +0.162 / +0.6% | +2.470 / +8.3% | +1.947 / +8.9% | +0.112 / +0.3% | +1.125 |
| B0b | +0.322 / +1.4% | +2.360 / +7.7% | +1.941 / +8.6% | +0.115 / +0.3% | +1.139 |
| R-1 | not read (R-1) | not read (R-1) | not read (R-1) | not read (R-1) | not read (R-1) |
| R-2 .. R-12 | pending | pending | pending | pending | pending |
| **V8-F** | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) |

V8-F year table in full: pending (no V8-F). B0c, the baseline, in full (return rows, net Sharpe, return, volatility, turnover, cost per traded dollar):

| year | return rows | net Sharpe | net return | volatility | tau mean | cost bps/$ |
|---|---|---|---|---|---|---|
| 2020 | 252 | +0.319 | +1.4% | 4.53% | 0.0362 | 15.67 |
| 2021 | 252 | +2.352 | +7.7% | 3.16% | 0.0335 | 12.03 |
| 2022 | 251 | +1.936 | +8.6% | 4.32% | 0.0332 | 12.80 |
| 2023 | 250 | +0.118 | +0.3% | 3.40% | 0.0333 | 12.16 |

Cumulative dSR V8-F vs B0c by year (`BUNDLE.years`): pending (no V8-F).

## 3. Cost and financing stresses (net SR by scenario)

Rows: V8-F, B0c, B0a. Every scenario prices the same construction (`NAVF.scenarios[]`, same for the other cells).

| cell | S1 linear 6 bps | **S2 modeled $1bn** | S2 x engine-tiers | S2 x flat-300 | S3 terminal-adverse (K = 1) | S2-FEE (G-3a, descriptive) |
|---|---|---|---|---|---|---|
| **V8-F** | pending (no V8-F) | **pending (no V8-F)** | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) |
| B0c | +1.295 | **+1.133** | +1.070 | +0.929 | +0.111 | +1.069 |
| B0a | +1.289 | **+1.125** | +1.068 | +0.924 | +0.201 | - |

Capacity (R-5 criterion, `CAP[R-5]`): pending (R-5 not run).
Capacity of B0c (E-29, report only, gates nothing; `build-equity/mega-nav-v8-b0c-dlret-ws60-v71-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247/v7_extras.json`; x1 = the primary S2 book bit for bit: True): net Sharpe at .5 / 1 / 2 / 4 / 8 x $1bn +1.174 / +1.133 / +1.085 / +0.978 / +0.851; cost bps per traded dollar 11.15 / 13.13 / 15.47 / 17.60 / 19.24. Ruling PM4-5: the capacity books scale only the impact law (m^0.5) and the participation cap (1/m); the aim is the $1bn book's at every multiple, so the 2x and 4x rows are slightly optimistic on the aim cap.

## 4. How the alphas become the book (final cell)

Pending: V8-F does not exist at the owner stop. The walk-through is written with the final scorecard.

## 5. Alpha table (final library, pending (no V8-F) candidates)

Status, HAC t and tau are the TRAIN admission statistics (`ADM.candidates[]`); weights from `W`. The traded-horizon
columns are **report only, gates nothing** (v8-prereg item 8): ic_theta and marginal IC21 from `CARDS.candidates[]`,
f_theta and its HAC t from `ADM.candidates[]`.

| id | theme | tier | raw dir | change vs v7.1 | status | HAC t | tau | **w** | ic_theta | f_theta (HAC t) | marginal IC21 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) | pending (no V8-F) |

Theme mass on the final role: pending (no V8-F).

## 6. Alpha DSL strings (verbatim from `LIB`)

Pending (no V8-F library).

## 7. Diagnostics G-1..G-3 (zero trials, descriptive; `DIAG`, run on B0c)

`DIAG` as one `diagnostics-v8.json` was not produced: E-18 ran the ten diagnostics on B0c as eight split runs, no tool verb combines them, and they are not merged (the pitch's diagnostics block is unavailable for this reason). Each row below reads `diagnostics.<id>` of the split file named in its last column; the eight files share one header.

Declaration: descriptive only (v8-prereg rule 8): no diagnostic gates, selects or re-weights anything; nothing here is an input to an admission, a composition, a fit or a construction. Window research-window-v2; tool sha256 2be3b164e449dfd5ac975de629797cee7d3a18e58af51313cc01bd2646d485a2. None gates,
selects or re-weights anything (v8-prereg item 8). A skipped diagnostic is listed with its reason.

| id | question | status | headline figures or reason | split file |
|---|---|---|---|---|
| G-1a | ic_theta, f_theta and marginal IC per member: which members contribute at the traded horizon | ok | weighted ic_theta 0.0037; negative at theta accruals, bac, bm, ind_adj_rev_5, ins_opp, iv_rv_spread, noa, smax5; unscored ebit_ev, fscore, gpa, opbe, opex_at, rd_me | `diagnostics-v8-g1a-g3d.json` |
| G-1b | turnover attribution, the planned turnover of each member's and theme's own aim: do the fast members carry most of the turnover | ok | combined turnover model / book book 0.0340; model 0.0317; fast members ind_adj_rev_5, iv_rv_spread, seasonality_same_month, ea_overdue own share 0.4544 | `diagnostics-v8-g1b-g1c.json` |
| G-1c | netting ratio, combined-score turnover over the sleeves' turnover: how much trades cancel | ok | netting ratio themes model / book 0.4250 / 0.4549 | `diagnostics-v8-g1b-g1c.json` |
| G-2a | ex-ante variance split of the book (market, industry, style, specific): can name-level risk sizing matter | ok | variance share market / industry / style / specific industry 0.1818; market 0.0053; specific 0.1556; style 0.6573 | `diagnostics-v8-g2a.json` |
| G-2b | IC by volatility tercile and by ADV tercile, IC in the top 1,000 by size: which alpha scaling holds, where capacity is | ok | weighted IC by vol / ADV tercile, top 1,000 adv high 0.0105; low 0.0110; mid 0.0101; all all 0.0112; size_top1000 rest 0.0127; top1000 0.0085; volatility high 0.0144; low 0.0089; mid 0.0103 | `diagnostics-v8-g2b.json` |
| G-2c | held dollars and aim dollars over ADV (p50, p95, max): does R-5 (Q = .10) bind | ok | held / ADV p95, max, share above Q cells 1,927,893; gross_share_above_q 0.0713; max 101.3; measured 1,927,893; no_adv 0; p50 0.0109; p95 0.0875; p99 0.1501; q 0.1000; share_above_q 0.0363; aim / ADV cells 1,844,715; gross_share_above_q 0.1108; max 0.5702; measured 1,844,715; no_adv 0; p50 0.0135; p95 0.1098; p99 0.1797; q 0.1000; share_above_q 0.0623 | `diagnostics-v8-g2c.json` |
| G-3a | borrow stress S2-FEE, fee by decile of SI / institutional ownership: how much of net Sharpe is a flat-fee artefact (descriptive; S2 stays primary) | ok | S2 net +1.133 -> S2-FEE +1.069 (delta -0.064) | `diagnostics-v8-g3a.json` |
| G-3b | low-risk members' IC before and after the beta / volatility (price-risk-v1) projection | ok | IC retained after price-risk-v1 bac 0.6846; smax 0.7142; smax5 1.2038 | `diagnostics-v8-g3b.json` |
| G-3c | book net Sharpe with the combined signal delayed 1, 2, 3 sessions: the cost of slower execution | ok | net Sharpe delta at delay 1 / 2 / 3 1 -0.0123; 2 -0.0322; 3 -0.0395 | `diagnostics-v8-g3c.json` |
| G-3d | hierarchical clusters of sleeve PnL correlations against the theme labels: are the themes separate bets | ok | adjusted Rand vs themes 0.1836; effective bets themes 4.8738 | `diagnostics-v8-g1a-g3d.json` |

## 8. Trial accounting (Appendix A)

```
TRAIN construction cells 40; admission trials this sprint 0; window research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); history reads 0; 2025+ never read.
Trial accounting (TRAIN 2020-2023 only; hidden 2024+ unread in this sprint; no validation statistic read):
  before v8: N 37 (ledger through v7.1); validation reads before v8: 2 (2023-2024); 2025+ never read.
  v8 re-base: B0a, B0b, B0c (N 38-40); window re-runs of ledgered cells add 0 (0 run: the PM5-22 re-runs are pending).
  v8 construction cells: R-1: constructed; mechanics fail (all-rows gross 1.0672, limit [.90, 1.05]); no return read; not ledgered; ruling open. R-2 .. R-12 pending -> N 40 (ledger).
  admission: 0 (R-2 not started) plus 0 `_f49` re-screens (ruling R2-e; none run).
  budget: N <= 51, admission <= 15 (plan 12.1): within (N 40, admission 0).
  Not trials: identity checks, window re-runs (item 2), invalid cells (item 7): none ledgered (0 window re-run lines, 0 defect lines; 1 protocol line).
  DSR (B0c, the last ledgered cell; V8-F pending; N = 40, T 1005) -- not meaningful yet: 3-cell variance, PM5-22:
    OD-4 variance (cells scored on research-window-v2: 3): DSR 0.984, SR0 0.0148 ann
    legacy variance (reported, gates nothing): DSR 0.707, SR0 0.850 ann
```

## 9. OD-1 disclosure (v8-prereg item 1)

1. Window. TRAIN is [2020-01-01, 2024-01-01). Hidden: 2024-01-01 and later. Owner ruling 2026-09-29.
   Disclosure: 2023 and 2024 were read twice at book level as validation in earlier sprints. 2023 statistics are
   therefore partly selected. 2025 and later has never been read.

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
| v6 3.1: make the latest 3-day announcement return half of the earnings theme | Latest announcement return is insignificant in the top 1000 since 2010 (1.15% per year, t 0.67, 976% turnover); the 12-month sum earns 4.90%, t 2.94, at 430% ([Gerard-Jehl 2025](https://www.quoniam.com/wp-content/uploads/2025/10/The-Many-Facets-of-Stock-Momentum.pdf)) | Contradicts |
| v6 3.6: replace `accruals` and `cfoa` with cash-based operating profitability | Operating profit with R&D added back subsumes the cash-based measure, fully after 2000 ([Novy-Marx-Medhat 2025](https://www.nber.org/system/files/working_papers/w33601/w33601.pdf)); within-school revision; blocked on missing COGS and SG&A fields | Contradicts |
| v6 3.5 and section 4: residual momentum prior gross Sharpe 0.4-0.7 | Firm-specific momentum 3.45% per year, t 1.53, after 2000; residual momentum is partly a bet against beta that the book's projection removes ([Ehsani-Linnainmaa](https://www.aeaweb.org/conference/2023/program/paper/8Ah8THYY)); suggested prior 0.2-0.4 [est] | Contradicts |
| v6 3.7 (multi-lag seasonality, lags 1 to 5) and v7 S6 (multi-lag seasonality listed as defensible) | CZ-own 2005-2024: lag 1 -.23, lags 2-5 -.03 ex-microcap and -.65 value-weighted (t -2.06); only lags 16-20 survive and cannot be built ([own computation](https://www.openassetpricing.com/data/)) | Contradicts, on the researcher's own computation |
| v6 section 4: "Composite issuance (5y) / XFIN", grade A- | XFIN is spanned (alpha t -.24, R2 .81); composite equity issuance keeps alpha (t 2.68); 5-year share growth sits between; grade B (CZ-own) | Updates: split the entry, lower the grade |
| v6 lever 2 and v7 R2.2: cost-shrunk targets, expected +0.06 to +0.15 | Tried and rejected at $1bn (cost per dollar -13 to -16%, gross Sharpe -10%); premia are about twice as strong in small caps ([Blitz-Baltussen-van Vliet 2020](https://repub.eur.nl/pub/130144/Repub_130144_O-A.pdf)); near break-even at $8bn [est] | Contradicts at $1bn; retest only at $4-8bn |
| v7 R2.1: alpha-maximising single-period optimiser, expected +0.05 to +0.15 | Tried and failed; diagnosis is a binding gross cap with alpha = IC x sigma x score [est]; replace with target tracking first, regularise jointly if retried ([Boyd et al. 2024](https://arxiv.org/pdf/2401.05080)) | Updates: change the formulation |
| v7 S2: no-trade band width scales as cost^(1/3) | cost^(1/2) with a stable forecast; band is asymmetric, trade to the near edge ([Isichenko 2021](https://arxiv.org/abs/2110.15239)) | Updates (qualifies) |
| v6 lever 6: volatility-scale the momentum sleeve, grade A- | Managed portfolios fail in real time and after costs except for momentum; earlier results carry look-ahead bias; the conditional rule halves turnover ([Bongaerts-Kang-van Dijk 2020](https://repub.eur.nl/pub/130215/Bongaerts-Kang-van-Dijk-Conditional-volatility-targeting-2020-FAJ.pdf)) | Updates: momentum sleeves only, conditional rule, small book effect |
| v6 lever 7: factor-momentum theme tilt, 0 to +0.05 | At most about +0.02 to +0.04 before added turnover [est]; v6's range is at the upper edge of what sources support | Updates downward; do not build |
| v6 lever 1: shrink the fast sleeves | A large-cap short-term composite with trading rules has net alpha ([Blitz et al. 2023](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=4115411)), but as a dedicated sleeve, not through a 21-day decay | Partly contradicts; scope-limited |
| v7 S4: net Sharpe 1.0 at about $5bn by extrapolation | Replayed capacity curve gives 1.08 at $8bn and about $11bn for Sharpe 1.0 by the square-root fit [est] | Updates upward |
| v7 S4: 13.73 bps impact at 2% of daily volume (fitted model) | 17 bps at 2% and 28 bps at 6% in binned live trades to 2013 ([Israel-Jiang-Ross 2017](https://www.aqr.com/-/media/AQR/Documents/Insights/Working-Papers/AQR--Craftsmanship-Alpha.pdf)); samples and definitions differ | Updates: use both as cost scenarios |
| v7 S3: hedging unpriced risk lifts squared Sharpe from 1.17 to 2.13 | 1.16 to 2.16 in the version the researcher read ([Daniel-Mota-Rottke-Santos](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3083143)); difference between paper versions | Updates (immaterial) |
| v7 R1.1 and R1.3: break-even about 10y for themes, 37y for 32 sleeves | 11.6y for ten themes and 44y for 38 sleeves [est]; blend weight 0.26 on four years [report arithmetic] | Updates; confirms equal weights as default |
| v7 R5.5: flat t >= 3 for mined or parameter-searched candidates | Hurdle scales with effective trials, 3.5 to 4.6 for 100 to 10,000; curated ideas need about 2.4-2.6 ([Harvey-Liu 2020](https://arxiv.org/pdf/2006.04269)) | Updates: three origin classes |
| v6 and v7: flat 50% post-publication haircut | Class-specific haircuts from 0-30% (profitability) to 80-100% (latest-surprise drift in the top 1000) [est] | Updates |
| v7 S6: opportunistic insider buying, prior gross Sharpe 0.3-0.6 | Low-reliability sources (a preprint and a vendor note) report abnormal returns 60-70% lower in 2008-2024 data and a routine/opportunistic split that does not reproduce ([arXiv 2602.06198](https://arxiv.org/html/2602.06198v1)) | Flag only; not strong enough to set a prior |
| v6 3.3: replace low-risk members with betting-against-correlation and scaled MAX, grade B+ | Supporting evidence uses the rank weighting criticised as hidden equal weighting ([Novy-Marx-Velikov 2022](https://mysimon.rochester.edu/novy-marx/research/BABAB.pdf)); no value-weighted or top-1000 test found | Updates: audit after projection before adding |
| v6 3.8: days-to-cover with a 63-126 day denominator | Result invariant to 1, 6 or 12-month turnover; use 126-252 days ([Hong et al. 2015](https://www.nber.org/system/files/working_papers/w21166/w21166.pdf)) | Confirms and extends |
| v6 section 4: three new independent themes add about 8% gross | Measured theme-proxy correlation .113; themes ten to twelve add about 0.05 [est] | Confirms |
| v6 lever R: name-level borrow fees are a must-do | Fee drag about 0.12 Sharpe units if shorts were spread evenly; modelled net Sharpe overstated by up to about 0.1 under a flat rate [est] | Confirms and quantifies |
| v6 lever X and v7 do-not-build: no fitted weights, no theme timing on short samples | Timing needs standalone Sharpe above 0.1 for any weight; fitted sleeve weights need 44 years | Confirms |

What v8 did about each "contradicts" row: pending (written with the final scorecard).
