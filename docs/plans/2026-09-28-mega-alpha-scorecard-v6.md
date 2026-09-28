# Mega-alpha scorecard v6 and alpha DSL (as of 2026-09-28)

Integration checkout `C:/atx-wt/pool-2`, branch `feat/aes-codex-integration-20260925`. All numbers are TRAIN 2020-2022 (no 2023+ file was read in v6). Primary cost scenario **S2** = `modeled-1bn-stale5-v1` impact costs at $1bn NAV x `swap-fin-v1` financing; daily rebalance (cadence 1); 252 sessions/yr; Sharpe on excess returns. Sources: `build-equity/mega-nav-v6-summ-n28.{txt,json}` (nav_summ over 13 v5 + 15 v6 cells, `--dsr-n 28`, reference = v5 REF `v5-ew-t.05-d.1-fixed`), per-cell runner receipts, `mega-weights-v6{l,u}-ew/`, the ledger `progress.md` and handoff 5 (`docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md`). Generator: `.superpowers/sdd/mega-alpha-20260926/studies/v6_scorecard.py`. The v5 scorecard (`2026-09-27-mega-alpha-scorecard.md`) stays the reference for library v4/v5.1.

## 1. Headline

| | cell | S2 net SR | gross SR | gross lev (all rows) | DSR N=28 cross-cell / Lo | status |
|---|---|---|---|---|---|---|
| **Final cell (V6-F)** | `v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` | **+1.182** | 1.547 | 0.971 | 0.904 / 0.499 | net >= 1.0 PASS; R6' mechanics PASS; pre-registered DSR >= .95 **FAIL** (.904) |
| Highest TRAIN net SR | `v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc` | **+1.192** | 1.542 | 0.778 | 0.907 / 0.505 | L 1: under-deployed (gross .78); fails mechanics |
| Best library v6 cell on role v2 | `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc` | **+0.975** | 1.304 | 0.761 | 0.839 / 0.364 | L 1: gross .76; fails mechanics |
| v5 deployable book (handoff 4) | `v5-ew-t.05-d.1-fixed-L1.279` | **+0.712** | 1.167 | 1.002 | 0.708 / 0.215 | v5 best mechanics-pass cell |
| Best out-of-sample (VAL 2023-2024, trial #2) | v4.1 `b2 f.25` | **+0.641** | 0.802 | 0.235 | | a 24%-gross book (ruling R-1) |
| VAL trial #1 | v3 (b1 f1) | -1.27 | | 0.763 | | TRAIN 1.81 -> VAL -1.27 (overfit) |

**The TRAIN objective is met, but the pre-registered freeze condition is not.** The final cell nets S2 +1.182 at all-rows gross 0.971, mean net +0.0053, tau 0.0375/0.0476. Its cross-cell DSR over N = 28 is 0.904 (SR0 0.385 ann), below the .95 required by the v6 prereg (V6-F). The Lo-null DSR is 0.499 (SR0 1.184 ann). No freeze is proposed by the controller; validation trial #3 is unspent and is the owner's call (U1). Paired vs the v5 REF: dSR +0.440, Memmel SE 0.254 (t 1.73), CBB 95% [+0.008, +0.944], LW p 0.082. Year net returns 2020 +0.1% / 2021 +8.3% / 2022 +8.2%: the 2020 contribution is about zero.

## 2. TRAIN cells (S2 primary)

### 2a. The 15 v6 cells

Rule aim-partial-v5, theta .05, dust .1, fixed rate unless named; neutralization price-risk-v1 unless named; library v6 (`5ee66d13`) and ew-theme-v1 on every cell except ew6. `x` = exit rate for nonmembers, `obdelta` = delta order basis, `loc` = locate-in-aim, `v6u` = linked-operating-v1 universe. Acceptance = sign of the paired dSR vs the named parent matches the pre-registered "+" prior (magnitude is not a criterion). dSR vs parent is the ledger value (Memmel SE). Gross/net leverage are means over every daily row (the R6' gate basis); vol is the S2 annualised net vol; cost bps/$ = `cost_bps_traded`. NAV exe = bounded-runner receipt.

| # | cell (`mega-nav-...`) | lever | S2 net | gross SR | HAC t | vol | tau mean/p95 | cost bps/$ | gross all rows | net all rows | dSR vs parent (SE) [parent] | verdict | NAV exe |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `v6l-ew-t.05-d.1-fixed` | V6-L library v6 (parent of the grid) | +0.915 | 1.313 | 1.68 | 3.71% | 0.0467/0.0637 | 12.56 | 0.7534 | +0.0124 | +0.156 (.189) [v51 ew (+0.759)] | ACCEPTED | `212d9e22` (v6-0) |
| 2 | `v6l-ew-t.05-d.1-fixed-x.05` | C2 exit .05, target orders | +0.938 | 1.324 | 1.72 | 3.76% | 0.0454/0.0622 | 12.14 | 0.7721 | +0.0197 | +0.023 (.050) [v6l parent] | accepted (sign) | `212d9e22` (v6-0) |
| 3 | `v6l-ew-t.05-d.1-fixed-x.1` | C2 exit .1, target orders | +0.927 | 1.314 | 1.71 | 3.74% | 0.0460/0.0627 | 12.16 | 0.7621 | +0.0158 | +0.012 (.030) [v6l parent] | accepted (sign) | `212d9e22` (v6-0) |
| 4 | `v6l-ew-t.05-d.1-fixed-obdelta` | C1 delta orders, exit 1 | +0.934 | 1.269 | 1.69 | 3.75% | 0.0400/0.0507 | 11.71 | 0.7573 | +0.0154 | +0.019 (.095) [v6l parent] | accepted (sign) | `212d9e22` (v6-0) |
| 5 | `v6l-ew-t.05-d.1-fixed-obdelta-x.05` | C1 delta + C2 exit .05 | +0.959 | 1.279 | 1.73 | 3.82% | 0.0389/0.0498 | 11.14 | 0.7748 | +0.0223 | +0.044 (.125) [v6l parent] | ACCEPTED (best grid cell) | `212d9e22` (v6-0) |
| 6 | `v6l-ew-t.05-d.1-fixed-obdelta-x.1` | C1 delta + C2 exit .1 | +0.951 | 1.274 | 1.72 | 3.79% | 0.0394/0.0501 | 11.24 | 0.7655 | +0.0186 | +0.036 (.114) [v6l parent] | accepted (sign) | `212d9e22` (v6-0) |
| 7 | `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc` | C3 locate-in-aim on delta x.05 | +0.975 | 1.304 | 1.77 | 3.62% | 0.0389/0.0499 | 11.15 | 0.7611 | +0.0040 | +0.060 (.101) [v6l parent] | ACCEPTED (fixes the |net| gate) | `212d9e22` (v6-0) |
| 8 | `v6l-ew-t.03-d.1-fixed-obdelta-x.05` | theta .03 on delta x.05 | +0.914 | 1.168 | 1.67 | 3.53% | 0.0306/0.0451 | 10.38 | 0.7084 | +0.0182 | -0.001 (.133) [v6l parent] | REJECTED (sign) | `212d9e22` (v6-0) |
| 9 | `v6l-ew-t.05-d.05-fixed-obdelta-x.05` | dust .05 on delta x.05 | +0.958 | 1.279 | 1.73 | 3.82% | 0.0391/0.0499 | 11.08 | 0.7764 | +0.0227 | +0.043 (.125) [v6l parent] | not adopted (-0.001 vs dust .1) | `212d9e22` (v6-0) |
| 10 | `v6l-ew-t.05-d.2-fixed-obdelta-x.05` | dust .2 on delta x.05 | +0.966 | 1.285 | 1.74 | 3.79% | 0.0381/0.0490 | 11.32 | 0.7696 | +0.0213 | +0.051 (.124) [v6l parent] | sign +, not carried (+0.007 vs dust .1; ruling) | `212d9e22` (v6-0) |
| 11 | `v6l-ew6-t.05-d.1-fixed-obdelta-x.05-loc` | V6-W ew-theme-v6 composition | +0.921 | 1.203 | 1.70 | 3.80% | 0.0293/0.0353 | 10.98 | 0.8243 | +0.0045 | -0.055 (.297) [loc cell] | REJECTED (sign) | `f55537fc` (v6-2) |
| 12 | `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-nind-v1` | C5 price-risk-ind-v1 (FF12 FWL) | +0.903 | 1.316 | 1.61 | 2.92% | 0.0394/0.0508 | 11.11 | 0.7601 | +0.0027 | -0.072 (.156) [loc cell] | REJECTED (sign) | `f55537fc` (v6-2) |
| 13 | `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-nind-v2` | C5 price-risk-ind-v2 (vol126/ladv252) | +0.927 | 1.332 | 1.66 | 2.96% | 0.0392/0.0511 | 11.00 | 0.7601 | +0.0031 | -0.048 (.176) [loc cell] | REJECTED (sign) | `f55537fc` (v6-2) |
| 14 | `v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc` | V6-U linked-operating-v1 universe | +1.192 | 1.542 | 2.10 | 3.67% | 0.0374/0.0474 | 12.82 | 0.7784 | +0.0038 | +0.217 (.227) [loc cell] | ACCEPTED | `f55537fc` (v6-2) |
| 15 | `v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` | V6-F final: V6-U stack at L 1.247 | **+1.182** | 1.547 | 2.08 | 4.57% | 0.0375/0.0476 | 13.55 | 0.9713 | +0.0053 | -0.010 (.007) [V6-U L 1] | FINAL CELL (C4 L re-derivation) | `f55537fc` (v6-2) |

Not trials (identity checks): `v6-ew-t.05-d.1-fixed-obtarget-x1` (D2: v6-0 default path, all 12 files byte-identical to `v51-ew-t.05-d.1-fixed`) and `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-v62` (branch review I1: the loc cell on v6-2, all 10 CSVs byte-identical to the v6-0 cell).

### 2b. All 28 cells: DSR, paired vs v5 REF, mechanics

"DSR x-cell" uses V[SR_n] across the 28 cells (1.403e-04 per session; SR0 .385 ann; nav_summ n28). "DSR Lo" is the Lo (2002) single-cell null at N = 28 recomputed from the n28 net moments with nav_summ's own formula (SR0 ~ the cell's own SR). dSR vs REF is paired against `v5-ew-t.05-d.1-fixed` (Memmel SE; Ledoit-Wolf studentized block-21 bootstrap, 2000 draws). Post-ramp gross = mean over rows after the first 63 (C4).

| cell | net SR | gross SR | HAC t | mu/yr | vol/yr | MDD | 2020 / 2021 / 2022 | gross all | gross post-ramp | net all | tau mean/p95 | cost bps/$ | held names | dSR vs REF (SE) | LW 95% (p) | DSR x-cell | DSR Lo | mechanics |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `v5-ew-t.05-d.1-fixed` | +0.742 | 1.177 | 1.46 | +2.53% | 3.41% | 3.3% | +0.2% / +3.9% / +3.4% | 0.782 | 0.801 | +0.0106 | 0.0433/0.0602 | 12.95 | 2909 | - | - | 0.725 | 0.230 | FAIL |
| `v5-ew-t.03-d.1-fixed` | +0.692 | 1.053 | 1.35 | +2.23% | 3.23% | 3.2% | -0.2% / +3.8% / +3.1% | 0.709 | 0.733 | +0.0065 | 0.0379/0.0580 | 12.35 | 2909 | -0.050 (0.080) | [-0.237, +0.137] (0.587) | 0.697 | 0.206 | FAIL |
| `v5-ew-t.08-d.1-fixed` | +0.681 | 1.193 | 1.34 | +2.44% | 3.57% | 3.5% | +0.1% / +3.8% / +3.4% | 0.844 | 0.858 | +0.0140 | 0.0490/0.0618 | 13.53 | 2908 | -0.061 (0.076) | [-0.237, +0.116] (0.485) | 0.691 | 0.200 | FAIL |
| `v5-ew-t.05-d0-fixed` | +0.724 | 1.178 | 1.43 | +2.48% | 3.42% | 3.3% | +0.3% / +3.9% / +3.1% | 0.781 | 0.800 | +0.0104 | 0.0464/0.0640 | 12.84 | 2916 | -0.018 (0.027) | [-0.078, +0.043] (0.520) | 0.715 | 0.221 | FAIL |
| `v5-ew-t.05-d.1-per-name` | +0.546 | 0.990 | 1.06 | +1.82% | 3.34% | 3.3% | -0.2% / +3.1% / +2.5% | 0.739 | 0.770 | +0.0079 | 0.0430/0.0666 | 14.36 | 2907 | -0.195 (0.096) | [-0.394, +0.003] (0.053) | 0.608 | 0.142 | FAIL |
| `v5-ew-t.05-d.1-fixed-L1.279` | +0.712 | 1.167 | 1.40 | +3.11% | 4.37% | 4.3% | +0.1% / +5.0% / +4.1% | 1.002 | 1.026 | +0.0148 | 0.0438/0.0609 | 13.64 | 2917 | -0.030 (0.013) | [-0.055, -0.005] (0.018) | 0.708 | 0.215 | PASS |
| `v5-aim-t.05-d.1-fixed` | +0.616 | 1.012 | 1.20 | +2.26% | 3.66% | 3.8% | -1.8% / +5.2% / +3.3% | 0.836 | 0.858 | +0.0116 | 0.0364/0.0552 | 13.62 | 2910 | -0.126 (0.112) | [-0.353, +0.102] (0.259) | 0.651 | 0.171 | FAIL |
| `v5-aim-t.03-d.1-fixed` | +0.643 | 0.983 | 1.24 | +2.25% | 3.50% | 3.7% | -1.7% / +5.1% / +3.4% | 0.775 | 0.803 | +0.0080 | 0.0330/0.0543 | 12.93 | 2909 | -0.099 (0.141) | [-0.398, +0.199] (0.506) | 0.667 | 0.183 | FAIL |
| `v5-aim-t.08-d.1-fixed` | +0.572 | 1.026 | 1.12 | +2.16% | 3.78% | 3.9% | -1.9% / +5.1% / +3.2% | 0.884 | 0.900 | +0.0148 | 0.0401/0.0549 | 14.25 | 2910 | -0.170 (0.119) | [-0.406, +0.065] (0.161) | 0.623 | 0.152 | FAIL |
| `v5-aim-t.05-d0-fixed` | +0.618 | 1.035 | 1.21 | +2.28% | 3.69% | 3.8% | -1.5% / +5.2% / +3.2% | 0.836 | 0.856 | +0.0112 | 0.0401/0.0599 | 13.37 | 2916 | -0.124 (0.121) | [-0.357, +0.109] (0.289) | 0.652 | 0.172 | FAIL |
| `v5-aim-t.05-d.1-per-name` | +0.449 | 0.864 | 0.87 | +1.58% | 3.52% | 3.9% | -2.0% / +4.3% / +2.4% | 0.794 | 0.828 | +0.0078 | 0.0363/0.0588 | 15.32 | 2908 | -0.293 (0.131) | [-0.561, -0.026] (0.032) | 0.543 | 0.108 | FAIL |
| `v5-aim-t.05-d.1-fixed-L1.279` | +0.607 | 1.022 | 1.18 | +2.84% | 4.69% | 4.9% | -2.3% / +6.6% / +4.2% | 1.072 | 1.099 | +0.0160 | 0.0371/0.0569 | 14.28 | 2918 | -0.135 (0.112) | [-0.359, +0.089] (0.220) | 0.645 | 0.167 | FAIL |
| `v51-ew-t.05-d.1-fixed` | +0.759 | 1.193 | 1.49 | +2.59% | 3.41% | 3.3% | +0.3% / +3.9% / +3.5% | 0.781 | 0.800 | +0.0107 | 0.0434/0.0603 | 12.94 | 2909 | +0.017 (0.011) | [-0.006, +0.040] (0.137) | 0.734 | 0.239 | FAIL |
| `v6l-ew-t.05-d.1-fixed` | +0.915 | 1.313 | 1.68 | +3.39% | 3.71% | 3.3% | +2.4% / +2.8% / +4.9% | 0.753 | 0.772 | +0.0124 | 0.0467/0.0637 | 12.56 | 2910 | +0.173 (0.191) | [-0.210, +0.557] (0.366) | 0.813 | 0.326 | FAIL |
| `v6l-ew-t.05-d.1-fixed-x.05` | +0.938 | 1.324 | 1.72 | +3.52% | 3.76% | 3.3% | +2.8% / +3.0% / +4.7% | 0.772 | 0.792 | +0.0197 | 0.0454/0.0622 | 12.14 | 3032 | +0.196 (0.206) | [-0.221, +0.613] (0.357) | 0.823 | 0.340 | FAIL |
| `v6l-ew-t.05-d.1-fixed-x.1` | +0.927 | 1.314 | 1.71 | +3.46% | 3.74% | 3.3% | +2.5% / +2.9% / +4.9% | 0.762 | 0.781 | +0.0158 | 0.0460/0.0627 | 12.16 | 2969 | +0.185 (0.198) | [-0.208, +0.579] (0.352) | 0.819 | 0.334 | FAIL |
| `v6l-ew-t.05-d.1-fixed-obdelta` | +0.934 | 1.269 | 1.69 | +3.50% | 3.75% | 3.4% | +2.5% / +3.1% / +4.9% | 0.757 | 0.779 | +0.0154 | 0.0400/0.0507 | 11.71 | 2910 | +0.192 (0.217) | [-0.247, +0.632] (0.393) | 0.822 | 0.338 | FAIL |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.05` | +0.959 | 1.279 | 1.73 | +3.66% | 3.82% | 3.3% | +3.0% / +3.2% / +4.8% | 0.775 | 0.798 | +0.0223 | 0.0389/0.0498 | 11.14 | 3027 | +0.217 (0.237) | [-0.270, +0.706] (0.385) | 0.833 | 0.353 | FAIL |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.1` | +0.951 | 1.274 | 1.72 | +3.60% | 3.79% | 3.3% | +2.7% / +3.2% / +4.9% | 0.765 | 0.788 | +0.0186 | 0.0394/0.0501 | 11.24 | 2966 | +0.209 (0.228) | [-0.250, +0.668] (0.375) | 0.830 | 0.348 | FAIL |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc` | +0.975 | 1.304 | 1.77 | +3.53% | 3.62% | 3.4% | +2.8% / +3.1% / +4.6% | 0.761 | 0.783 | +0.0040 | 0.0389/0.0499 | 11.15 | 3057 | +0.233 (0.218) | [-0.169, +0.636] (0.243) | 0.839 | 0.364 | FAIL |
| `v6l-ew-t.03-d.1-fixed-obdelta-x.05` | +0.914 | 1.168 | 1.67 | +3.23% | 3.53% | 3.3% | +2.2% / +2.9% / +4.5% | 0.708 | 0.737 | +0.0182 | 0.0306/0.0451 | 10.38 | 3015 | +0.172 (0.226) | [-0.289, +0.633] (0.467) | 0.813 | 0.326 | FAIL |
| `v6l-ew-t.05-d.05-fixed-obdelta-x.05` | +0.958 | 1.279 | 1.73 | +3.66% | 3.82% | 3.3% | +3.0% / +3.1% / +4.7% | 0.776 | 0.799 | +0.0227 | 0.0391/0.0499 | 11.08 | 3075 | +0.216 (0.237) | [-0.274, +0.707] (0.388) | 0.833 | 0.352 | FAIL |
| `v6l-ew-t.05-d.2-fixed-obdelta-x.05` | +0.966 | 1.285 | 1.74 | +3.66% | 3.79% | 3.3% | +3.0% / +3.1% / +4.8% | 0.770 | 0.792 | +0.0213 | 0.0381/0.0490 | 11.32 | 2979 | +0.224 (0.236) | [-0.259, +0.708] (0.366) | 0.836 | 0.357 | FAIL |
| `v6l-ew6-t.05-d.1-fixed-obdelta-x.05-loc` | +0.921 | 1.203 | 1.70 | +3.50% | 3.80% | 3.5% | +0.2% / +5.2% / +5.1% | 0.824 | 0.848 | +0.0045 | 0.0293/0.0353 | 10.98 | 3058 | +0.179 (0.249) | [-0.304, +0.662] (0.452) | 0.814 | 0.331 | FAIL |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-nind-v1` | +0.903 | 1.316 | 1.61 | +2.63% | 2.92% | 3.2% | +0.4% / +4.3% / +3.1% | 0.760 | 0.782 | +0.0027 | 0.0394/0.0508 | 11.11 | 3055 | +0.161 (0.234) | [-0.236, +0.559] (0.422) | 0.807 | 0.320 | FAIL |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-nind-v2` | +0.927 | 1.332 | 1.66 | +2.74% | 2.96% | 3.2% | +0.3% / +4.5% / +3.5% | 0.760 | 0.782 | +0.0031 | 0.0392/0.0511 | 11.00 | 3051 | +0.185 (0.238) | [-0.233, +0.604] (0.394) | 0.818 | 0.334 | FAIL |
| `v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc` | +1.192 | 1.542 | 2.10 | +4.38% | 3.67% | 3.7% | +0.1% / +6.6% / +6.6% | 0.778 | 0.802 | +0.0038 | 0.0374/0.0474 | 12.82 | 1842 | +0.450 (0.254) | [-0.042, +0.942] (0.072) | 0.907 | 0.505 | FAIL |
| **`v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`** | +1.182 | 1.547 | 2.08 | +5.41% | 4.57% | 4.7% | +0.1% / +8.3% / +8.2% | 0.971 | 1.001 | +0.0053 | 0.0375/0.0476 | 13.55 | 1851 | +0.440 (0.254) | [-0.050, +0.930] (0.082) | 0.904 | 0.499 | PASS |

The n28 `netting_ratio` column is not reproduced: the run passed the v6u weights to every cell, so the ratio is matched only for the two v6u cells (the others print UNMATCHED).

## 3. Cost and financing stresses (net SR by scenario)

Rows: the final cell, its L 1 parent (V6-U), the loc cell (best role-v2 construction), the V6-L parent and the v5 references. Every scenario prices the same shared construction (locate-in-aim applies to every book).

| cell | S1 linear 6 bps | **S2 modeled $1bn** | S2 x engine-tiers | S2 x flat-300 | S3 terminal-adverse (K = 1) |
|---|---|---|---|---|---|
| **`v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`** | +1.324 | **+1.182** | +1.128 | +1.006 | +0.337 |
| `v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc` | +1.320 | **+1.192** | +1.138 | +1.019 | +0.347 |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc` | +1.091 | **+0.975** | +0.907 | +0.815 | +0.165 |
| `v6l-ew-t.05-d.1-fixed` | +1.089 | **+0.915** | +0.836 | +0.629 | +0.128 |
| `v51-ew-t.05-d.1-fixed` | +0.950 | **+0.759** | +0.668 | +0.471 | -0.087 |
| `v5-ew-t.05-d.1-fixed-L1.279` | +0.924 | **+0.712** | +0.619 | +0.428 | -0.136 |
| `v5-ew-t.05-d.1-fixed` | +0.933 | **+0.742** | +0.651 | +0.458 | -0.102 |

All 15 v6 cells (S1 / S2 / tiers / flat-300 / S3):

| cell | S1 | S2 | tiers | flat-300 | S3 |
|---|---|---|---|---|---|
| `v6l-ew-t.05-d.1-fixed` | +1.089 | +0.915 | +0.836 | +0.629 | +0.128 |
| `v6l-ew-t.05-d.1-fixed-x.05` | +1.097 | +0.938 | +0.851 | +0.655 | +0.131 |
| `v6l-ew-t.05-d.1-fixed-x.1` | +1.083 | +0.927 | +0.845 | +0.642 | +0.133 |
| `v6l-ew-t.05-d.1-fixed-obdelta` | +1.071 | +0.934 | +0.859 | +0.666 | +0.149 |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.05` | +1.071 | +0.959 | +0.880 | +0.705 | +0.162 |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.1` | +1.063 | +0.951 | +0.873 | +0.689 | +0.164 |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc` | +1.091 | +0.975 | +0.907 | +0.815 | +0.165 |
| `v6l-ew-t.03-d.1-fixed-obdelta-x.05` | +0.985 | +0.914 | +0.838 | +0.672 | +0.145 |
| `v6l-ew-t.05-d.05-fixed-obdelta-x.05` | +1.070 | +0.958 | +0.878 | +0.709 | +0.158 |
| `v6l-ew-t.05-d.2-fixed-obdelta-x.05` | +1.076 | +0.966 | +0.887 | +0.716 | +0.169 |
| `v6l-ew6-t.05-d.1-fixed-obdelta-x.05-loc` | +1.010 | +0.921 | +0.840 | +0.751 | +0.162 |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-nind-v1` | +1.047 | +0.903 | +0.815 | +0.708 | -0.085 |
| `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-nind-v2` | +1.059 | +0.927 | +0.832 | +0.737 | -0.046 |
| `v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc` | +1.320 | +1.192 | +1.138 | +1.019 | +0.347 |
| `v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` | +1.324 | +1.182 | +1.128 | +1.006 | +0.337 |

S3 writes delisted holdings off adversely; delisting returns are not modelled in S1/S2 (lane parked: T33a NO-GO, owner gate U3). S3 is positive on every v6 cell except the two C5 (industry-neutral) cells; the V6-L parent was the first positive S3 of any cell (+0.128) and the two V6-U cells reach +0.34.

## 4. How the alphas become the book (final cell)

1. **Library v6** `atx-impl/strategies/fund_industry_ic_v6.json` (sha `5ee66d13`, recipe `36c08452`, generator `generate_fund_ic_v6.py`): 38 candidates in 9 themes, each one DSL expression evaluated daily per name by the atx-engine alpha DSL VM. Vs v5.1: +`value_composite`, +`res_mom_12_1`; `cfoa`->`cbop`, `low_beta`->`bac`, `low_max`->`smax`; -`low_ivol`, -`lowvol_ind`; 27 members switched from `decay_linear(R(x),21)` to `R(decay_linear(x,21))` (removes the 21-session membership blackout); the 4 fast sleeves (standalone tau >= .08 in v5.1: `ind_adj_rev_5`, `seasonality_same_month`, `si_change`, `iv_rv_spread`) lose the decay.
2. **Universe `linked-operating-v1`** (V6-U; role `recent-fast-train-2020-2022-v2-lo1`, manifest `3e79978a`): a base member (top 3000 by prior-63-session $ADV, price > $5) is kept only if it has exactly one point-in-time identity-bridge link on the issuer's primary line, class_status common, and a visible SIC (lag 1 session, <= 550 days old) outside [6189, 6221, 6722, 6726, 6770, 6792, 6795] (pooled vehicles, blank checks, royalty trusts; REITs stay). Score-window dropped member share 0.403; dropped cells by reason (score window): {"ambiguous": 0, "class_not_common": 0, "no_visible_sic": 34, "non_operating_sic": 5343, "secondary_line": 3068, "unlinked": 899105}; kept member cells 1,940,364 of 3,220,647; min kept members per scored day 1675. Fields are rebuilt on the restricted role (`lo1-fields-v6b`, manifest `c69b9c0f`, 40 fields): `mkt_ret` and group ranks are now over the restricted members. The bridge is the r4 rehearsal (`scope_complete false`): unbridged operating stocks drop; ADRs of linked filers stay.
3. **Admission** `v4-prior-v1` re-run on the restricted role (TRAIN only): literature prior sign embedded in the DSL, never flipped; reject if redundant (|rho| > .90 vs a stronger member by (tier, roster order)), turnover > .7, HAC t < -2.0 (veto) or insufficient data. Restricted role: 31 of 38 admitted (6 redundant, 1 veto: `si_change`). Role v2: 30 of 38.
4. **Composition** `ew-theme-v1` (weights `490c3836`): 9 themes at 1/9, equal weight within a theme across its admitted members; no mean or covariance estimation. weighted standalone turnover 0.1172. `ew-theme-v6` (drop low_risk, merge options into short_interest, fast x1/3) was tested and rejected (V6-W).
5. **Combined signal**: per candidate, unsigned centered tied rank over members, times the prior sign, weighted sum (`mega-v6uw-train-ew-1/train_combined.json`, sha `1e146796`; IC exe `b1c1ba07`).
6. **Desired target**: tied rank of the combined signal; **locate-in-aim** (C3): a member in the special borrow tier at decision d with a negative desired weight is set to 0 *before* neutralisation (about 100 names per decision on role v2); then `price-risk-v1` (OLS residual on [1, z beta252, z vol63, z log ADV63], rescaled to the zeroed entry gross); gross 1; nonmembers 0.
7. **Construction** `aim-partial-v5`: each session a member moves next = cur + theta (L x desired - cur) unless |L x desired - cur| <= dust / N_d; theta .05, dust .1, fixed rate, **L 1.247** (= 1 / post-ramp gross .8019 of the L 1 parent, C4). **Exit rate** (C2): a present nonmember decays next = cur x (1 - .05) and snaps to 0 inside the dust band; an absent nonmember exits at once. **Delta orders** (C1): a changed nonzero plan becomes an order for (planned - current) x NAVpost; price drift between decision and fill rides instead of being traded back. Liquidity cache on (fixed-rate path bit-identical).
8. **Execution and costs**: decide at d after the mark, fill at the close of d+1, first return row d+2. S2 = `sqrt-impact-v1` at $1bn (half-spread 5 bps + commission 1 bp + Y .6 x sigma x sqrt(participation), max participation 1% of ADV63) x `swap-fin-v1` (ACT/360: long +40 bps, short +20 bps + tier fee GC 30 / warm 100 / special 500 bps; special shorts blocked). Result: held names 1851, held share 1.041 (exit decay lingers ~60 sessions), tau 0.0375, cost 13.55 bps per traded $.

## 5. Alpha table (library v6, 38 candidates)

Every candidate embeds prior sign +1 (raw literature direction in column "raw dir"). "lo1" = the restricted role (the final cell's admission and weights, `mega-weights-v6u-ew`); "v2" = role v2 (`mega-weights-v6l-ew`, the V6-L / V6-C cells). HAC t and tau are the TRAIN admission statistics of the standalone neutralized gross-1 factor (forward r[d+2]; tau = daily one-way turnover). `reject_redundant` names the member it duplicates.

| id | theme | tier | raw dir | change vs v5.1 | status lo1 | HAC t lo1 | tau lo1 | **w_ew lo1** | status v2 | HAC t v2 | w_ew v2 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `value_composite` | value | B+ | +1 | added | admitted | +0.88 | 0.0272 | **0.0222** | admitted | +0.71 | 0.0222 |
| `bm` | value | B | +1 | R(decay(x)) | admitted | +0.30 | 0.0268 | **0.0222** | admitted | +0.35 | 0.0222 |
| `ep` | value | B | +1 | R(decay(x)) | reject_redundant (ebit_ev) | +0.75 | 0.0271 | **0.0000** | reject_redundant (ebit_ev) | +0.84 | 0.0000 |
| `cfp` | value | B+ | +1 | R(decay(x)) | reject_redundant (value_composite) | +0.83 | 0.0272 | **0.0000** | reject_redundant (value_composite) | +0.91 | 0.0000 |
| `fcfp` | value | B+ | +1 | R(decay(x)) | reject_redundant (value_composite) | +0.79 | 0.0292 | **0.0000** | reject_redundant (value_composite) | +0.58 | 0.0000 |
| `ebit_ev` | value | B+ | +1 | R(decay(x)) | admitted | +0.41 | 0.0271 | **0.0222** | admitted | +0.44 | 0.0222 |
| `net_payout` | value | A | +1 | R(decay(x)) | admitted | +0.35 | 0.0274 | **0.0222** | admitted | +0.14 | 0.0222 |
| `sp` | value | B | +1 | R(decay(x)) | reject_redundant (value_composite) | +0.96 | 0.0243 | **0.0000** | reject_redundant (value_composite) | +0.97 | 0.0000 |
| `rd_me` | value | B- | +1 | R(decay(x)) | admitted | +0.36 | 0.0345 | **0.0222** | admitted | +0.56 | 0.0222 |
| `gpa` | profitability_quality | A | +1 | R(decay(x)) | admitted | +1.50 | 0.0241 | **0.0139** | admitted | +1.35 | 0.0139 |
| `opbe` | profitability_quality | A- | +1 | R(decay(x)) | admitted | +0.29 | 0.0266 | **0.0139** | admitted | -0.14 | 0.0139 |
| `cbop` | profitability_quality | A- | +1 | replaces cfoa | admitted | +2.29 | 0.0253 | **0.0139** | admitted | +1.83 | 0.0139 |
| `roe_q` | profitability_quality | A- | +1 | R(decay(x)) | admitted | +1.09 | 0.0322 | **0.0139** | admitted | +0.61 | 0.0139 |
| `roa` | profitability_quality | B+ | +1 | R(decay(x)) | admitted | +0.58 | 0.0273 | **0.0139** | admitted | +0.10 | 0.0139 |
| `accruals` | profitability_quality | C+ | -1 | R(decay(x)) | admitted | +1.30 | 0.0269 | **0.0139** | admitted | +1.35 | 0.0139 |
| `fscore` | profitability_quality | C+ | +1 | R(decay(x)) | admitted | +0.39 | 0.0391 | **0.0139** | admitted | +0.09 | 0.0139 |
| `opex_at` | profitability_quality | B | +1 | R(decay(x)) | admitted | +3.16 | 0.0222 | **0.0139** | admitted | +3.39 | 0.0139 |
| `asset_growth` | investment_issuance | C+ | -1 | R(decay(x)) | admitted | +0.95 | 0.0282 | **0.0370** | admitted | +0.90 | 0.0556 |
| `noa` | investment_issuance | B | -1 | R(decay(x)) | admitted | +1.70 | 0.0231 | **0.0370** | admitted | +1.87 | 0.0556 |
| `issuance_xbrl` | investment_issuance | A | -1 | R(decay(x)) | admitted | +1.04 | 0.0266 | **0.0370** | reject_redundant (net_payout) | +0.82 | 0.0000 |
| `issuance_vendor` | investment_issuance | A- | -1 | R(decay(x)) | reject_redundant (net_payout) | +0.20 | 0.0277 | **0.0000** | reject_redundant (net_payout) | -0.07 | 0.0000 |
| `sue` | earnings_momentum | C+ | +1 | R(decay(x)) | admitted | +0.53 | 0.0302 | **0.0278** | admitted | +0.25 | 0.0278 |
| `droe` | earnings_momentum | B | +1 | R(decay(x)) | admitted | +0.37 | 0.0315 | **0.0278** | admitted | +0.15 | 0.0278 |
| `chtax` | earnings_momentum | B | +1 | R(decay(x)) | admitted | +0.82 | 0.0289 | **0.0278** | admitted | +0.62 | 0.0278 |
| `ear` | earnings_momentum | B+ | +1 | R(decay(x)) | admitted | -0.74 | 0.0359 | **0.0278** | admitted | -0.78 | 0.0278 |
| `res_mom_12_1` | price_momentum | B+ | +1 | added | admitted | +0.87 | 0.0351 | **0.0278** | admitted | +1.30 | 0.0278 |
| `mom_12_1` | price_momentum | B+ | +1 | R(decay(x)) | reject_redundant (res_mom_12_1) | +1.12 | 0.0381 | **0.0000** | reject_redundant (res_mom_12_1) | +1.52 | 0.0000 |
| `ind_mom_12_1` | price_momentum | B | +1 | unchanged | admitted | +1.88 | 0.0471 | **0.0278** | admitted | +1.83 | 0.0278 |
| `within_ind_mom` | price_momentum | C+ | +1 | unchanged | admitted | +0.33 | 0.0323 | **0.0278** | admitted | +0.19 | 0.0278 |
| `high_52w` | price_momentum | A- | +1 | R(decay(x)) | admitted | -0.54 | 0.0506 | **0.0278** | admitted | -0.19 | 0.0278 |
| `bac` | low_risk | B+ | -1 | replaces low_beta | admitted | +0.96 | 0.0417 | **0.0556** | admitted | +1.55 | 0.0556 |
| `smax` | low_risk | B+ | -1 | replaces low_max | admitted | +1.13 | 0.0836 | **0.0556** | admitted | +0.82 | 0.0556 |
| `si_ratio` | short_interest | B+ | -1 | R(decay(x)) | admitted | -1.13 | 0.0318 | **0.0556** | admitted | -1.34 | 0.0556 |
| `dtc` | short_interest | B+ | -1 | R(decay(x)) | admitted | +0.09 | 0.0465 | **0.0556** | admitted | -0.67 | 0.0556 |
| `si_change` | short_interest | B- | -1 | decay removed (fast sleeve) | reject_veto | -2.90 | 0.1179 | **0.0000** | reject_veto | -2.23 | 0.0000 |
| `ind_adj_rev_5` | reversal_seasonality | B- | -1 | decay removed (fast sleeve) | admitted | +1.28 | 0.5856 | **0.0556** | admitted | +1.48 | 0.0556 |
| `seasonality_same_month` | reversal_seasonality | C+ | +1 | decay removed (fast sleeve) | admitted | +1.44 | 0.2897 | **0.0556** | admitted | +1.49 | 0.0556 |
| `iv_rv_spread` | options_implied | B | +1 | decay removed (fast sleeve) | admitted | +1.04 | 0.3603 | **0.1111** | admitted | +1.18 | 0.1111 |

Removed v5.1 ids: `low_ivol` (removed); `lowvol_ind` (removed); `cfoa` (replaced by cbop); `low_beta` (replaced by bac); `low_max` (replaced by smax).

Theme mass on the restricted role (ew-theme-v1): value 5 x 0.0222; profitability_quality 8 x 0.0139; investment_issuance 3 x 0.0370; earnings_momentum 4 x 0.0278; price_momentum 4 x 0.0278; low_risk 2 x 0.0556; short_interest 2 x 0.0556; reversal_seasonality 2 x 0.0556; options_implied 1 x 0.1111.

## 6. Alpha DSL strings (verbatim from `fund_industry_ic_v6.json`)


### value

Price-scaled fundamentals (book, earnings, cash flow, free cash flow, EBIT/EV, net payout, sales, R&D) and a composite of the book, earnings and cash-flow yields, ranked within FF12 industry; high value predicts higher returns.

- **`value_composite`** (admitted; w_ew lo1 0.0222; added) - Fama and French (1992, JF); Lakonishok, Shleifer and Vishny (1994, JF); Israel, Laursen and Richardson (2021, JPM) composite value; Asness and Frazzini (2013, JPM) current price
  ```
  (((group_rank(decay_linear((be / me_company), 21), grp_ff12) + group_rank(decay_linear((ni_ttm / me_company), 21), grp_ff12)) + group_rank(decay_linear((cfo_ttm / me_company), 21), grp_ff12)) / 3)
  ```
- **`bm`** (admitted; w_ew lo1 0.0222; R(decay(x))) - Rosenberg, Reid and Lanstein (1985, JPM); Fama and French (1992, JF); robust pre-1963 (Linnainmaa and Roberts 2018), weak 2017-2020
  ```
  group_rank(decay_linear(((be / me_company) + (0 * log(be))), 21), grp_ff12)
  ```
- **`ep`** (reject_redundant (ebit_ev); w_ew lo1 0.0000; R(decay(x))) - Basu (1977, JF); Fama and French (1992, JF) E/P for positive earnings
  ```
  group_rank(decay_linear(((ni_ttm / me_company) + (0 * log(ni_ttm))), 21), grp_ff12)
  ```
- **`cfp`** (reject_redundant (value_composite); w_ew lo1 0.0000; R(decay(x))) - Lakonishok, Shleifer and Vishny (1994, JF) cash flow to price; operating cash flow as in Desai, Rajgopal and Venkatachalam (2004, TAR)
  ```
  group_rank(decay_linear(((cfo_ttm / me_company) + (0 * log(cfo_ttm))), 21), grp_ff12)
  ```
- **`fcfp`** (reject_redundant (value_composite); w_ew lo1 0.0000; R(decay(x))) - Lakonishok, Shleifer and Vishny (1994, JF); free cash flow yield (Hackel, Livnat and Rai 1994, FAJ)
  ```
  group_rank(decay_linear(((cfo_ttm - capx_ttm) / me_company), 21), grp_ff12)
  ```
- **`ebit_ev`** (admitted; w_ew lo1 0.0222; R(decay(x))) - Loughran and Wellman (2011, JFQA) enterprise multiple (works in large caps)
  ```
  group_rank(decay_linear((((oi_ttm / ((me_company + debt) - che)) + (0 * log(oi_ttm))) + (0 * log(((me_company + debt) - che)))), 21), grp_ff12)
  ```
- **`net_payout`** (admitted; w_ew lo1 0.0222; R(decay(x))) - Boudoukh, Michaely, Richardson and Roberts (2007, JF) net payout yield
  ```
  group_rank(decay_linear((((dvc_ttm + prstkc_ttm) - sstk_ttm) / me_company), 21), grp_ff12)
  ```
- **`sp`** (reject_redundant (value_composite); w_ew lo1 0.0000; R(decay(x))) - Barbee, Mukherji and Raines (1996, FAJ) sales to price
  ```
  group_rank(decay_linear(((sale_ttm / me_company) + (0 * log(sale_ttm))), 21), grp_ff12)
  ```
- **`rd_me`** (admitted; w_ew lo1 0.0222; R(decay(x))) - Chan, Lakonishok and Sougiannis (2001, JF) R&D to market equity
  ```
  group_rank(decay_linear(((xrd_ttm / me_company) + (0 * log(xrd_ttm))), 21), grp_ff12)
  ```

### profitability_quality

Profitability and quality (gross, operating, cash-based operating, ROE, ROA, low accruals, F-score) and operating leverage (operating costs to assets), ranked within FF12 industry; high quality and high operating leverage predict higher returns.

- **`gpa`** (admitted; w_ew lo1 0.0139; R(decay(x))) - Novy-Marx (2013, JFE) gross profitability (works in large caps; replicated by Hou, Xue and Zhang 2020)
  ```
  group_rank(decay_linear(((gp_ttm / at) + (0 * log(at))), 21), grp_ff12)
  ```
- **`opbe`** (admitted; w_ew lo1 0.0139; R(decay(x))) - Fama and French (2015, JFE) operating profitability (RMW)
  ```
  group_rank(decay_linear(((oi_ttm / be) + (0 * log(be))), 21), grp_ff12)
  ```
- **`cbop`** (admitted; w_ew lo1 0.0139; replaces cfoa) - Ball, Gerakos, Linnainmaa and Nikolaev (2016, JFE) cash-based operating profitability (subsumes accruals); operating profitability excludes R&D: Ball, Gerakos, Linnainmaa and Nikolaev (2015, JFE)
  ```
  group_rank(decay_linear((((cfo_ttm + (power(xrd_ttm, (1 - ts_count_nans(xrd_ttm, 1))) - ts_count_nans(xrd_ttm, 1))) / at) + (0 * log(at))), 21), grp_ff12)
  ```
- **`roe_q`** (admitted; w_ew lo1 0.0139; R(decay(x))) - Hou, Xue and Zhang (2015, RFS) q-factor ROE
  ```
  group_rank(decay_linear(((ni_q / be_lag1q) + (0 * log(be_lag1q))), 21), grp_ff12)
  ```
- **`roa`** (admitted; w_ew lo1 0.0139; R(decay(x))) - Balakrishnan, Bartov and Faurel (2010, JAE); Chen, Novy-Marx and Zhang (2011, WP)
  ```
  group_rank(decay_linear(((ni_ttm / at) + (0 * log(at))), 21), grp_ff12)
  ```
- **`accruals`** (admitted; w_ew lo1 0.0139; R(decay(x))) - Sloan (1996, TAR); Hribar and Collins (2002, JAR) cash-flow-statement accruals; decay: Green, Hand and Soliman (2011, MS)
  ```
  group_rank(decay_linear(((-1 * ((ni_ttm - cfo_ttm) / ((at + at_lag4) / 2))) + (0 * log(((at + at_lag4) / 2)))), 21), grp_ff12)
  ```
- **`fscore`** (admitted; w_ew lo1 0.0139; R(decay(x))) - Piotroski (2000, JAR) F-score
  ```
  group_rank(decay_linear(fscore, 21), grp_ff12)
  ```
- **`opex_at`** (admitted; w_ew lo1 0.0139; R(decay(x))) - Novy-Marx (2011, RF) operating leverage; proxy opex = sale_ttm - oi_ttm (includes D&A) because fields-v6 has no opex_ttm
  ```
  group_rank(decay_linear((((sale_ttm - oi_ttm) / at) + (0 * log(at))), 21), grp_ff12)
  ```

### investment_issuance

Low asset growth, low net operating assets and low share issuance (XBRL and split-neutral vendor), ranked within FF12 industry; low investment and issuance predict higher returns.

- **`asset_growth`** (admitted; w_ew lo1 0.0370; R(decay(x))) - Cooper, Gulen and Schill (2008, JF) asset growth (weak in big stocks: Fama and French 2008)
  ```
  group_rank(decay_linear(((-1 * ((at / at_lag4) - 1)) + (0 * log(at_lag4))), 21), grp_ff12)
  ```
- **`noa`** (admitted; w_ew lo1 0.0370; R(decay(x))) - Hirshleifer, Hou, Teoh and Zhang (2004, JAE) net operating assets
  ```
  group_rank(decay_linear(((-1 * (noa / at_lag4)) + (0 * log(at_lag4))), 21), grp_ff12)
  ```
- **`issuance_xbrl`** (admitted; w_ew lo1 0.0370; R(decay(x))) - Pontiff and Woodgate (2008, JF) share issuance; Daniel and Titman (2006, JF); present in big stocks (Fama and French 2008)
  ```
  group_rank(decay_linear((-1 * log((shrs_q / shrs_q_lag4))), 21), grp_ff12)
  ```
- **`issuance_vendor`** (reject_redundant (net_payout); w_ew lo1 0.0000; R(decay(x))) - Daniel and Titman (2006, JF) composite equity issuance over one year; Pontiff and Woodgate (2008, JF)
  ```
  group_rank(decay_linear((-1 * log((((shares_out * raw_close) / close) / delay(((shares_out * raw_close) / close), 252)))), 21), grp_ff12)
  ```

### earnings_momentum

Earnings news: SUE, change in ROE, change in tax expense and the 3-day earnings announcement return; good news predicts continuation.

- **`sue`** (admitted; w_ew lo1 0.0278; R(decay(x))) - Bernard and Thomas (1989, JAR); Livnat and Mendenhall (2006, JAR); filing-clock lag, decay: Martineau (2022, CFR)
  ```
  rank(decay_linear(sue, 21))
  ```
- **`droe`** (admitted; w_ew lo1 0.0278; R(decay(x))) - Hou, Mo, Xue and Zhang (2021, RF) change in ROE; Balakrishnan, Bartov and Faurel (2010, JAE)
  ```
  rank(decay_linear(((((ni_q / be_lag1q) - (ni_q_lag4 / be_lag1q_lag4)) + (0 * log(be_lag1q))) + (0 * log(be_lag1q_lag4))), 21))
  ```
- **`chtax`** (admitted; w_ew lo1 0.0278; R(decay(x))) - Thomas and Zhang (2011, JAR) tax expense surprises
  ```
  rank(decay_linear((((txt_q - txt_q_lag4) / at_lag4) + (0 * log(at_lag4))), 21))
  ```
- **`ear`** (admitted; w_ew lo1 0.0278; R(decay(x))) - Chan, Jegadeesh and Lakonishok (1996, JF) earnings announcement return; Brandt, Kishore, Santa-Clara and Venkatachalam (2008, WP)
  ```
  rank(decay_linear(ts_backfill((ts_sum((((close / delay(close, 1)) - 1) - mkt_ret), 3) + (0 * log((earn_recent * delay(earn_recent, 1))))), 126), 21))
  ```

### price_momentum

12-1 momentum, 12-1 residual momentum, industry 12-1 momentum, within-industry momentum and nearness to the 52-week high; winners continue.

- **`res_mom_12_1`** (admitted; w_ew lo1 0.0278; added) - Blitz, Huij and Martens (2011, JEF) residual momentum; Blitz, Hanauer and Vidojevic (2020, JEF) idiosyncratic momentum; Grundy and Martin (2001, RFS)
  ```
  rank(decay_linear(delay((((ts_sum(((close / delay(close, 1)) - 1), 231) / stddev(((close / delay(close, 1)) - 1), 231)) - (correlation(((close / delay(close, 1)) - 1), mkt_ret, 231) * (ts_sum(mkt_ret, 231) / stddev(mkt_ret, 231)))) / signedpower(abs((1 - (correlation(((close / delay(close, 1)) - 1), mkt_ret, 231) * correlation(((close / delay(close, 1)) - 1), mkt_ret, 231)))), 0.5)), 21), 21))
  ```
- **`mom_12_1`** (reject_redundant (res_mom_12_1); w_ew lo1 0.0000; R(decay(x))) - Jegadeesh and Titman (1993, JF); 12-1 as in Fama-French UMD; crash risk: Daniel and Moskowitz (2016, JFE)
  ```
  rank(decay_linear(((delay(close, 21) / delay(close, 252)) - 1), 21))
  ```
- **`ind_mom_12_1`** (admitted; w_ew lo1 0.0278; unchanged) - Moskowitz and Grinblatt (1999, JF) industry momentum
  ```
  decay_linear(rank(group_mean(((delay(close, 21) / delay(close, 252)) - 1), grp_ff49)), 21)
  ```
- **`within_ind_mom`** (admitted; w_ew lo1 0.0278; unchanged) - Asness, Porter and Stevens (2000, WP) within-industry momentum
  ```
  decay_linear(rank(group_neutralize(((delay(close, 21) / delay(close, 252)) - 1), grp_ff49)), 21)
  ```
- **`high_52w`** (admitted; w_ew lo1 0.0278; R(decay(x))) - George and Hwang (2004, JF) 52-week high
  ```
  rank(decay_linear((close / ts_max(close, 252)), 21))
  ```

### low_risk

Betting against correlation (low correlation with the market) and low scaled MAX (largest daily return per unit of volatility); low risk predicts higher risk-adjusted returns.

- **`bac`** (admitted; w_ew lo1 0.0556; replaces low_beta) - Asness, Frazzini, Gormsen and Pedersen (2020, JFE) betting against correlation; correlation of overlapping 3-day returns: Frazzini and Pedersen (2014, JFE)
  ```
  rank(decay_linear((-1 * correlation(ts_sum(((close / delay(close, 1)) - 1), 3), ts_sum(mkt_ret, 3), 250)), 21))
  ```
- **`smax`** (admitted; w_ew lo1 0.0556; replaces low_max) - Asness, Frazzini, Gormsen and Pedersen (2020, JFE) scaled MAX (SMAX); Bali, Cakici and Whitelaw (2011, JFE) MAX
  ```
  rank(decay_linear((-1 * ((ts_max(((close / delay(close, 1)) - 1), 21) / stddev(((close / delay(close, 1)) - 1), 252)) + (0 * log(stddev(((close / delay(close, 1)) - 1), 252))))), 21))
  ```

### short_interest

FINRA short interest ratio, days to cover and one-month change in short interest; heavy or rising shorting predicts lower returns.

- **`si_ratio`** (admitted; w_ew lo1 0.0556; R(decay(x))) - Asquith, Pathak and Ritter (2005, JFE); Boehmer, Huszar and Jordan (2010, JFE)
  ```
  rank(decay_linear((-1 * (si_shares / shares_out)), 21))
  ```
- **`dtc`** (admitted; w_ew lo1 0.0556; R(decay(x))) - Hong, Li, Ni, Scheinkman and Yan (2015, WP) days to cover
  ```
  rank(decay_linear((-1 * si_dtc), 21))
  ```
- **`si_change`** (reject_veto; w_ew lo1 0.0000; decay removed (fast sleeve)) - Rapach, Ringgenberg and Zhou (2016, JFE) short interest predicts lower returns (direction)
  ```
  rank((-1 * ((si_shares / shares_out) - delay((si_shares / shares_out), 21))))
  ```

### reversal_seasonality

Industry-adjusted short-term reversal and same-calendar-month seasonality.

- **`ind_adj_rev_5`** (admitted; w_ew lo1 0.0556; decay removed (fast sleeve)) - Da, Liu and Schaumburg (2014, MS); Hameed and Mian (2015, JFQA) within-industry reversal
  ```
  rank((-1 * group_neutralize(((close / delay(close, 5)) - 1), grp_ff49)))
  ```
- **`seasonality_same_month`** (admitted; w_ew lo1 0.0556; decay removed (fast sleeve)) - Heston and Sadka (2008, JFE) seasonality
  ```
  rank(((delay(close, 231) / delay(close, 252)) - 1))
  ```

### options_implied

Implied-minus-realized volatility spread; high implied volatility relative to realized volatility predicts higher returns.

- **`iv_rv_spread`** (admitted; w_ew lo1 0.1111; decay removed (fast sleeve)) - Bali and Hovakimian (2009, MS) volatility spreads: realized minus implied volatility predicts lower returns
  ```
  rank((ts_backfill((iv_atm_21d + (0 * log(((iv_atm_21d - 0.02) * (5 - iv_atm_21d))))), 5) - (stddev(((close / delay(close, 1)) - 1), 21) * 15.874507866387544)))
  ```

## 7. Trial accounting (Appendix A)

```
Trial accounting (TRAIN 2020-2022 only; no 2023+ read in v6; per-candidate VAL statistics never read):
  v3 era: admission 48 + 121; composition 4 + 7; construction 14.
  since run #1 to v5: libraries v4 (37), v4.2 (40), v5.1 (38); compositions v4, v4.2, ew-theme-aim-v1, v5.1 x2;
    construction v4 1 + v4.1 grid 5 + v4.2 2 + v5 grid 10 + 2 L re-run + 1 v5.1; studies T26 5 paper books, T16,
    T28 audit; stat-arb cluster study (separate family, negative).
  v6: admission 38 (library v6 on role v2; res_mom_12_1 = re-trial of v4.2) + 38 (re-admission on the
    linked-operating-v1 role); compositions 3 (ew-theme-v1 on library v6 x2 [role v2, role lo1], ew-theme-v6);
    universe 1 (linked-operating-v1); construction cells 15 (V6-L parent, 5 C1/C2 grid, 4 conditional [loc,
    theta .03, dust .05, dust .2], ew6, ind-v1, ind-v2, V6-U, V6-F); studies: Phase A reviews and the v6-explore
    aggregation read existing TRAIN outputs (disclosed diagnostics).
  Not trials: D2 identity cell (v6-0 default path = v51 cell, 12/12 files identical); I1 identity cell (-v62,
    10/10 CSVs identical); m1 IC re-run (mega-v6lw-train-ew-2, 6/6 train_combined.* identical); refused or
    colliding runs that produced no statistic (v6l u-run1..3 cache mismatch; v6u u-run1..4 field list / dir
    collisions; ew6 w-run1..3 admit refusal).
  validation: #1 (v3, book level), #2 (v4.1, 24%-gross book); #3 unspent (needs U1).
  DSR (final cell, N = 28 = 13 v5 + 15 v6 NAV cells, T 754, skew -1.319, kurtosis 13.54):
    cross-cell V[SR_n] = 1.403e-04/session, SR0 0.385 ann -> DSR 0.904  (prereg V6-F requires >= .95: FAIL)
    Lo (2002) null V = (1 + SR^2/2)/T, SR0 1.184 ann -> DSR 0.499
    N counts NAV cells only; it omits the 38 + 38 admission trials and the library-design degrees of freedom.
```
