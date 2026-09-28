# Mega-alpha — handoff 5 (v6 "reach net SR 1.0" sprint closed on TRAIN, 2026-09-28)

Author: v6 controller (Claude Opus 5.5, SDD), docs lane. Integration checkout `C:/atx-wt/pool-2`, branch
`feat/aes-codex-integration-20260925` (HEAD at writing `903c09bf`). Pre-registration: `v4-prereg.md` section
"## v6 revision" (commit `e0dfb8c7`). Ledger (newest first): `.superpowers/sdd/mega-alpha-20260926/progress.md`.
Whole-branch review: `task-V6-branch-review.md`. Scorecard: `docs/plans/2026-09-28-mega-alpha-scorecard-v6.md`
(generator `studies/v6_scorecard.py`). Cross-cell numbers: `build-equity/mega-nav-v6-summ-n28.{txt,json}`.
Trust the ledger and `git log` over this summary.

## 0. TL;DR

- **The TRAIN objective is met, but the pre-registered freeze condition is not.**
  - Final cell `mega-nav-v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`: **S2 net SR +1.182** at all-rows gross
    **.971** (post-ramp 1.001). gross SR 1.547, HAC t 2.08, mu 5.41%/yr, vol 4.57%, MDD 4.7%.
  - R6' mechanics PASS: gross in [.90, 1.05]; mean net +.0053; tau mean .0375 / p95 .0476.
  - Stresses: S1 +1.324, engine-tiers +1.128, flat-300 +1.006, S3 terminal-adverse +0.337.
  - The v6 prereg (V6-F) also requires a cross-cell DSR >= .95 with N = 13 + all v6 cells. Over N = 28 it is
    **.904** (SR0 .385 ann). The Lo (2002) null gives **.499** (SR0 1.184). **So the controller makes no freeze
    proposal.** Validation trial #3 is unspent, and whether to spend it on this cell, with DSR .904 disclosed, is
    the owner's decision (U1, §6).
- **What the book is made of** (each step paired against its own parent; the acceptance rule was the sign of dSR
  only):

  | step | S2 net | paired dSR (SE) | t |
  |---|---|---|---|
  | v5.1 ew parent | .759 | | |
  | + library v6 (`5ee66d13`: +value_composite, +res_mom_12_1, cfoa->cbop, low_beta->bac, low_max->smax, -low_ivol, -lowvol_ind; `R(decay(x))`; fast sleeves undecayed) | .915 | +0.156 (.189) | .83 |
  | + delta orders + exit rate .05 (C1, C2) | .959 | +0.044 (.125) | .35 |
  | + locate-in-aim (C3; vs V6-L parent) | .975 | +0.060 (.101) | .59 |
  | + linked-operating-v1 universe (V6-U; re-admission, re-fit ew-theme-v1, `mkt_ret` over restricted members) | 1.192 | +0.217 (.227) | .95 |
  | + L 1.247 = 1 / post-ramp gross .8019 (C4) | **1.182** | -0.010 (.007) | |

  End to end against the v5 REF (+0.742): dSR **+0.440**, Memmel SE .254, t 1.73. CBB 95% is [+.008, +.944],
  which excludes 0. The LW studentized bootstrap gives p .082.
- **Rejected** (sign against the prior):
  - ew-theme-v6 composition: -0.055 (SE .297).
  - price-risk-ind-v1: -0.072 (.156).
  - price-risk-ind-v2: -0.048 (.176).
  - theta .03: -0.001 (.133).

  **Not adopted:** dust .05 (-0.001 vs dust .1). dust .2 had the right sign (+0.007 vs dust .1) but was not carried
  into the final cell, by ruling.
- **Honesty caveats (read before U1):**
  1. Acceptance was sign-only. Every accepted step is inside one SE (|t| < 1). The cumulative +0.44 is
     t 1.73: suggestive, not established.
  2. Everything is TRAIN 2020-2022: in sample, max-selected over 28 NAV cells plus 76 admission trials (38 + 38)
     and the library-design choices. DSR N counts the NAV cells only.
  3. **The 2020 alpha is ~0 in the final cell.** Year net returns are 2020 +0.1%, 2021 +8.3%, 2022 +8.2%. The
     role-v2 loc cell had 2020 +2.8%. The universe restriction lifted 2021-22 and removed 2020, so the TRAIN Sharpe
     rests on two years.
  4. **Cost per $ rises with L.** At L 1.247 it is 13.55 bps per traded $ vs 12.82 at L 1: impact is convex. A
     larger AUM or a gross above 1 loses more.
  5. V6-U = universe + market proxy + re-admission, not the universe alone (D14). It depends on the r4 rehearsal
     identity bridge (`scope_complete false`).
- **Branch:** the whole-branch review verdict FIX REQUIRED (evidence only) is now **MERGE-READY**. The root closed
  I1 and m1 with byte-identity re-runs (§5), and 9 minors are carried as disclosures D1-D15. The merge is owner gate
  U5.

## 1. What was done (goal 2: code review + literature review -> build -> net SR >= 1.0)

| Task | Result | Evidence |
|---|---|---|
| Phase A: v6-review-signal (read-only) | 0C / 5I / 7m: 39.4% of member cells unlinked (ETF/SPAC/ADR ranked on price signals only); low_risk projected out by price-risk-v1; 21-session blackout from `decay(rank)`; coverage-weighted themes; double smoothing | `v6-code-review-signal.md` (`e0dfb8c7`) |
| Phase A: v6-review-exec (read-only) | Cost drag = trading .30 SR + financing .13 SR. Fixed-dollar orders trade back 17-18% of executed $ as drift (F1). Nonmember exits at rate 1 = 19-22% of planned turnover (F2). Locate block pushes mean net to +.0148 (F4). L calibrated on ramp-deflated mean (F5). | `v6-code-review-exec.md` |
| Phase A: v6-lit (web, cited) | 542 lines. Stacked levers est. +0.12..+0.30. Base-rate caution: net SR >= 1 at $1bn is at the upper edge of the literature. | `v6-literature.md` |
| v6-explore (read-only) | aggregation of existing TRAIN NAV outputs (cost / alpha by bucket); disclosed diagnostic, not a trial | ledger "v6 START" |
| Prereg `## v6 revision` | V6-C (C1-C5), V6-U, V6-W, V6-L, V6-F declared before any v6 TRAIN read | `e0dfb8c7`; briefs `04e9d5bc` |
| V6-L library v6 (pool-5) | 38 candidates (5 new or replacing, 31 smoothing-changed, 2 unchanged, 5 v5.1 ids removed). Review APPROVED 0C/0I/7m. | `9d302e4c` -> pool-2 `d6a0065f`; `task-V6L-*.md` |
| V6-C1 NAV order basis / exit rate / locate-in-aim / liquidity cache + nav_summ post-ramp (pool-10) | review FIX REQUIRED (the only Important was a test UB) -> fix 1 -> re-review ALL ADDRESSED | `ee574c4f`, `709beb69`; fix `144071a3`, `64e5fe98`; `task-V6C1*.md` |
| V6-C2 price-risk-ind-v1/v2, grp_ff12 plumbing, NAV reserve (pool-11) | review APPROVED (conditional, 0C/3I root gates) -> rebase onto C1 (5 conflicts + I2 reserve) | `720a0066` -> rebase `82255021`, `e5e8eb26`; `task-V6C2-*.md` |
| V6-W ew-theme-v6 fitter + within-theme redistribution (C++) + role `--universe linked-operating-v1` (pool-4) | review FIX REQUIRED 0C/3I/10m -> fix 1 (schema v2 gate, royalty trusts, numpy-2) -> re-review ALL ADDRESSED -> fix 2 (test fixture) | `4d239a9d`..`a48677cb`, `fba18d60`; `task-V6W*.md` |
| Build v6-0 (NAV) | source `ad31e817` (0 dirty), 47.8 s, 5 TUs. **NAV exe `212d9e22`.** Target tests 67/68: NavV6.RecipeSummaryKeysAndCliRefusals failed on the test UB fixed in C1 fix 1. | `mega-v6-0-receipt.json` |
| D2 identity (v6-0 default path) | `mega-nav-v6-ew-t.05-d.1-fixed-obtarget-x1` has 12/12 files byte-identical to `mega-nav-v51-ew-t.05-d.1-fixed` (re-verified by sha256 in this lane). Not a trial. | ledger "Build v6-0" |
| Build v6-1 (IC) | source `a48677cb` with **1 dirty entry** in the receipt, 47.4 s, 6 TUs. **IC exe `b1c1ba07`.** IC tests 69/70; the theme-refusal fixture was fixed by W fix 2. | `mega-v6-1-receipt.json` |
| Build v6-2 (NAV + tests) | source `e5e8eb26` (0 dirty), 27.7 s, 8 TUs. **NAV exe `f55537fc`.** Target tests 81/81, IC tests 70/70. | `mega-v6-2-receipt.json` |
| TRAIN: V6-L cell, C1/C2 grid (5), conditional (4), ew6, C5 x2, V6-U, V6-F | 15 cells, all first bounded pass, 27.7-73.0 s, <= 390 MiB NAV | §2 |
| Whole-branch adversarial review (Opus) | 0C / 1I / 9m. Verdict FIX REQUIRED (evidence only), then MERGE-READY after I1 and m1. | `task-V6-branch-review.md`; §5 |
| I1 / m1 identity re-runs | loc cell on v6-2: 10/10 CSVs identical. v6l ew weighted pass on `b1c1ba07`: 6/6 `train_combined.*` identical. Not trials. | §5 |
| Cross-cell summary + gate read-out | nav_summ over 28 cells: DSR .904 < .95, so no freeze proposal | `mega-nav-v6-summ-n28.*`; ledger "GATE READ-OUT" |

## 2. v6 TRAIN table (S2 = modeled-1bn-stale5-v1 x swap-fin-v1; 2020-2022; $1bn; cadence 1)

Common settings:
- Every cell uses library v6 (`5ee66d13`), ew-theme-v1, aim-partial-v5, theta .05, dust .1, fixed rate,
  price-risk-v1 and the liquidity cache, unless the lever column says otherwise.
- dSR vs parent is the ledger's paired value (Memmel SE), taken from each cell's own nav_summ run.
- Gross and net are means over all rows, which is the R6' gate basis.

Full 28-cell table (DSR both benchmarks, paired vs v5 REF with LW, years, post-ramp gross): scorecard §2b.

| # | cell (`mega-nav-...`) | lever | S2 net | gross SR | HAC t | vol | tau mean/p95 | cost bps/$ | gross all rows | net all rows | dSR vs parent (SE) [parent] | verdict | NAV exe |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `v6l-ew-t.05-d.1-fixed` | V6-L library v6 (parent of the grid) | +0.915 | 1.313 | 1.68 | 3.71% | 0.0467/0.0637 | 12.56 | 0.7534 | +0.0124 | +0.156 (.189) [v51 ew (+0.759)] | ACCEPTED | `212d9e22` (v6-0) |
| 2 | `v6l-ew-t.05-d.1-fixed-x.05` | C2 exit .05, target orders | +0.938 | 1.324 | 1.72 | 3.76% | 0.0454/0.0622 | 12.14 | 0.7721 | +0.0197 | +0.023 (.050) [v6l parent] | accepted (sign) | `212d9e22` (v6-0) |
| 3 | `v6l-ew-t.05-d.1-fixed-x.1` | C2 exit .1, target orders | +0.927 | 1.314 | 1.71 | 3.74% | 0.0460/0.0627 | 12.16 | 0.7621 | +0.0158 | +0.012 (.030) [v6l parent] | accepted (sign) | `212d9e22` (v6-0) |
| 4 | `v6l-ew-t.05-d.1-fixed-obdelta` | C1 delta orders, exit 1 | +0.934 | 1.269 | 1.69 | 3.75% | 0.0400/0.0507 | 11.71 | 0.7573 | +0.0154 | +0.019 (.095) [v6l parent] | accepted (sign) | `212d9e22` (v6-0) |
| 5 | `v6l-ew-t.05-d.1-fixed-obdelta-x.05` | C1 delta + C2 exit .05 | +0.959 | 1.279 | 1.73 | 3.82% | 0.0389/0.0498 | 11.14 | 0.7748 | +0.0223 | +0.044 (.125) [v6l parent] | ACCEPTED (best grid cell; breaches abs net .02) | `212d9e22` (v6-0) |
| 6 | `v6l-ew-t.05-d.1-fixed-obdelta-x.1` | C1 delta + C2 exit .1 | +0.951 | 1.274 | 1.72 | 3.79% | 0.0394/0.0501 | 11.24 | 0.7655 | +0.0186 | +0.036 (.114) [v6l parent] | accepted (sign) | `212d9e22` (v6-0) |
| 7 | `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc` | C3 locate-in-aim on delta x.05 | +0.975 | 1.304 | 1.77 | 3.62% | 0.0389/0.0499 | 11.15 | 0.7611 | +0.0040 | +0.060 (.101) [v6l parent] | ACCEPTED (fixes the abs net gate) | `212d9e22` (v6-0) |
| 8 | `v6l-ew-t.03-d.1-fixed-obdelta-x.05` | theta .03 on delta x.05 | +0.914 | 1.168 | 1.67 | 3.53% | 0.0306/0.0451 | 10.38 | 0.7084 | +0.0182 | -0.001 (.133) [v6l parent] | REJECTED (sign) | `212d9e22` (v6-0) |
| 9 | `v6l-ew-t.05-d.05-fixed-obdelta-x.05` | dust .05 on delta x.05 | +0.958 | 1.279 | 1.73 | 3.82% | 0.0391/0.0499 | 11.08 | 0.7764 | +0.0227 | +0.043 (.125) [v6l parent] | not adopted (-0.001 vs dust .1) | `212d9e22` (v6-0) |
| 10 | `v6l-ew-t.05-d.2-fixed-obdelta-x.05` | dust .2 on delta x.05 | +0.966 | 1.285 | 1.74 | 3.79% | 0.0381/0.0490 | 11.32 | 0.7696 | +0.0213 | +0.051 (.124) [v6l parent] | sign +, not carried (+0.007 vs dust .1; ruling) | `212d9e22` (v6-0) |
| 11 | `v6l-ew6-t.05-d.1-fixed-obdelta-x.05-loc` | V6-W ew-theme-v6 composition | +0.921 | 1.203 | 1.70 | 3.80% | 0.0293/0.0353 | 10.98 | 0.8243 | +0.0045 | -0.055 (.297) [loc cell] | REJECTED (sign) | `f55537fc` (v6-2) |
| 12 | `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-nind-v1` | C5 price-risk-ind-v1 (FF12 FWL) | +0.903 | 1.316 | 1.61 | 2.92% | 0.0394/0.0508 | 11.11 | 0.7601 | +0.0027 | -0.072 (.156) [loc cell] | REJECTED (sign) | `f55537fc` (v6-2) |
| 13 | `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-nind-v2` | C5 price-risk-ind-v2 (vol126/ladv252) | +0.927 | 1.332 | 1.66 | 2.96% | 0.0392/0.0511 | 11.00 | 0.7601 | +0.0031 | -0.048 (.176) [loc cell] | REJECTED (sign) | `f55537fc` (v6-2) |
| 14 | `v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc` | V6-U linked-operating-v1 universe | +1.192 | 1.542 | 2.10 | 3.67% | 0.0374/0.0474 | 12.82 | 0.7784 | +0.0038 | +0.217 (.227) [loc cell] | ACCEPTED | `f55537fc` (v6-2) |
| 15 | `v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` | V6-F final: V6-U stack at L 1.247 | **+1.182** | 1.547 | 2.08 | 4.57% | 0.0375/0.0476 | 13.55 | 0.9713 | +0.0053 | -0.010 (.007) [V6-U L 1] | FINAL CELL (C4 L re-derivation) | `f55537fc` (v6-2) |
| D2 | `v6-ew-t.05-d.1-fixed-obtarget-x1` | v6-0 default path on library v5.1 | +0.759 | 1.193 | | | | | .7812 | | identical book | not a trial (12/12 files = v51 cell) | `212d9e22` (v6-0) |
| I1 | `v6l-ew-t.05-d.1-fixed-obdelta-x.05-loc-v62` | cell 7 re-run on v6-2 | +0.975 | | | | | | | | identical book | not a trial (10/10 CSVs = cell 7) | `f55537fc` (v6-2) |

Only cell 15 passes R6' mechanics. Cells 1-14 run at L 1 with gross .71-.82. The exit-rate cells without
locate-in-aim push mean net to +.016-.023: cells 5, 9 and 10 exceed .02, and cell 2 sits at .0197. That is why C3 was
required. Cross-cell DSR at N = 28:
- final cell .904;
- V6-U L 1 .907;
- loc cell .839;
- v5 REF .725.

Lo-null DSR at N = 28:
- final cell .499;
- V6-U .505;
- loc cell .364;
- v5 REF .230.

## 3. Stack recipe (reproduce the final cell)

| pin | sha256 |
|---|---|
| library `atx-impl/strategies/fund_industry_ic_v6.json` | `5ee66d137c60527c4b00f948e8b15c04f2bddc789e988b660a4254a6df6e896a` |
| recipe `fund_industry_ic_v6.recipe.json` | `36c084522237b756d2847bc3e589855fafe589bba55b5111642c444afecfc72b` |
| base role `recent-fast-train-2020-2022-v2/manifest.json` | `210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de` |
| base fields `recent-fast-train-2020-2022-v2-fields-v6/manifest.json` (restriction crosscheck) | `32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8` |
| identity bridge `identity-bridge-r4-v1/manifest.json` | `ddf9716459a1116b85f713ca9cb788c3db753a6e1fea8eba335ed34320baebaa` |
| fundamental events `fundamental-events-v2/manifest.json` (SIC clock) | `74ed9a50ea686e0b0842ff9b09e78d6653ddeedd0d42f37893873ce269e3dd71` |
| **role lo1** `recent-fast-train-2020-2022-v2-lo1/manifest.json` (linked-operating-v1) | `3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809` |
| **fields lo1-fields-v6b** `recent-fast-train-2020-2022-v2-lo1-fields-v6b/manifest.json` (40 fields) | `c69b9c0faf76e333f69b480a698e57b1309f2882b74a40092d9ebb91b9fa82c8` |
| u pass orientations `mega-v6u-train-u-5/orientations.json` | `84d72e6ce4b388727fc40303fefe893da3e9ede5058d9878fa0851374ca1650e` |
| admission `mega-weights-v6u-ew/admission.json` | `78ccf155428bb086e16120727ab71262bd9fcd25b9b3ccc5d94716e8b2ff763a` |
| **weights** `mega-weights-v6u-ew/composition_weights.json` (ew-theme-v1, schema v1) | `490c3836e8e5f9dfc3a58cce43c707310010dbf46c54c4d08e0dfdb698522d48` |
| weights role v2 (cells 1-10, 12-13) `mega-weights-v6l-ew/composition_weights.json` | `b900602d7aa006941ecb8ebb94c5ba86979b1109ed9e88bc3b88e3d73a294f18` |
| ew-theme-v6 weights (cell 11, rejected) | `a24ca2054ddf71fc0dc94d9bc39f3a768cf9978f06c2883b52f8430083b1fbba` |
| fitter script (`fit_composition_weights.py`, from provenance) | `3acb6b3b34a3c2d656c60e1f96c6919b76904ddd8ba5f186b53fa8d76ee55bea` |
| combined `mega-v6uw-train-ew-1/train_combined.json` | `1e146796fd5f31246b0b37a3b697799e29a0462f4536826bc321f5ba9a1bc1be` |
| **IC exe** `atx-equity-strategy-ic.exe` (v6-1) | `b1c1ba0766f4628a445ab4e09abfaf9e766e8c84cbfef6d5526b4dc66c511bd8` |
| **NAV exe** `atx-equity-strategy-targets.exe` (v6-2) | `f55537fcca4fdf8ad10fe6dd098c97130da804e66be7f7cf3f795d4d9fa0fd66` |
| final cell `recipe.json` / summary `recipe_sha256` | `a790d577...` / `e86afc81...` |
| final cell `daily_modeled-1bn-stale5-v1+swap-fin-v1.csv` | `e930ad2f8961ac2aaa0e19736326cd67896c63f4dd5fae8436fe9003c0d32a3a` |

Steps, as root, on a clean tree:
1. Run `.superpowers/sdd/mega-alpha-20260926/studies/v6u_train.sh` with the phases `restrict`, `gate`, `fields`,
   `u`, `fit` and `w`. The script pins every input and never overwrites an output.
   - The `fields` step runs outside the bounded runner. It uses builder caps of 700 MiB and 1800 s, and it is a
     disclosed data step.
   - Its external sources (FINRA SI, TickerHistory3.parquet) are sha-recorded in the fields manifest.
2. Run the final NAV. `bash v6u_train.sh nav` with `LEV=1.247 DSR_N=28 REFN=<V6-U L 1 dir>` builds this command
   under the bounded runner (180 s / 1536 MiB):

```
atx-equity-strategy-targets.exe nav --combined build-equity/mega-v6uw-train-ew-1/train_combined.json
  --combined-sha256 1e146796... --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --role-sha256 3e79978a...
  --fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v6b/manifest.json --fields-sha256 c69b9c0f...
  --output build-equity/mega-nav-v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247 --rule aim-partial-v5 --cadence 1
  --trade-fraction .05 --dust-multiple .1 --aim-leverage 1.247 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30
  --neutralize price-risk-v1 --max-bytes 1073741824 --order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache
```

   Receipt: source `016ef599`, exit 0, 27.7 s, 339 MiB.
3. Summarize the cell:
   `nav_summ.py --weights build-equity/mega-weights-v6u-ew/composition_weights.json --reference <dir> --dsr-n 28 <dirs>`.
   The n28 run used `--reference build-equity/mega-nav-v5-ew-t.05-d.1-fixed` with all 28 dirs; its provenance
   records git head `fb8a8822` and script sha `d6e929d7`.

## 4. Trial accounting (Appendix A, cumulative)

```
Trial accounting (TRAIN 2020-2022 only; no 2023+ read in v6; per-candidate VAL statistics never read):
  v3 era: admission 48 + 121; composition 4 + 7; construction 14.
  since run #1 to v5: libraries v4 (37), v4.2 (40), v5.1 (38); compositions v4, v4.2, ew-theme-aim-v1, v5.1 x2
    (ew-theme-v1, ew-theme-aim-v1); construction v4 1 + v4.1 grid 5 + v4.2 2 + v5 grid 10 + 2 L re-run + 1 v5.1;
    studies T26 5 paper books, T16, T28 audit; stat-arb cluster study (separate family, negative).
  v6: admission 38 (library v6 on role v2; res_mom_12_1 = re-trial of the v4.2 candidate) + 38 (re-admission of the
    same 38 on the linked-operating-v1 role); compositions 3 (ew-theme-v1 on library v6 x2 [role v2, role lo1],
    ew-theme-v6 on library v6); universe 1 (linked-operating-v1); construction cells 15 (V6-L parent, 5 C1/C2 grid,
    4 conditional [loc, theta .03, dust .05, dust .2], ew6, ind-v1, ind-v2, V6-U, V6-F);
    studies: Phase A reviews and the v6-explore aggregation read existing TRAIN outputs (disclosed diagnostics).
  Not trials (reproductions / no statistic): D2 identity cell (v6-0 default path = v51 cell, 12/12 files);
    I1 identity cell (-v62, 10/10 CSVs); m1 IC re-run (mega-v6lw-train-ew-2, 6/6 train_combined.*);
    refused or colliding runs (v6l u-run1..3 cache mismatch; v6u u-run1..4 field list / dir collisions;
    ew6 w-run1..3 admit refusal).
  validation: #1 (v3, book level), #2 (v4.1, 24%-gross book); #3 UNSPENT (needs U1).
  DSR, final cell (N = 28 = 13 v5 + 15 v6 NAV cells; T 754; skew -1.319; kurtosis 13.54):
    cross-cell  V[SR_n] = 1.403e-04 per session (28 per-session SRs), SR0 0.385 ann -> DSR 0.904
                (prereg V6-F requires >= .95: FAIL)
    Lo (2002)   V = (1 + SR^2/2)/T, SR0 1.184 ann (~ the cell's own SR) -> DSR 0.499
    Scope: N counts NAV cells only; it omits the 38 + 38 admission trials and the library-design degrees of
    freedom (V6-L was written with the v5.1 TRAIN scorecard visible). The construction cells are correlated around
    rho .95, so independence is conservative there and optimistic for the signal-level search (review §7.2).
```

Handoff 4's v5 block used N = 13, V[SR_n] 3.083e-05 and REF .342 (Lo). At N = 28 the cross-cell variance is 4.5x
larger, because the 28 cells span net .449-1.192. The v5 REF cell falls to .725 cross-cell and .230 Lo.

## 5. Whole-branch review (T41-style, Opus 5.5) and its closure

`task-V6-branch-review.md`, run over `b1887951..ce10f008` (23 code files, +7308/-196):
- **Counts:** 0 Critical, 1 Important, 9 Minor.
- **Verdict:** FIX REQUIRED, evidence only (no code change).
- **Traced correct:** every path the accepted stack exercises. This covers delta orders at DECIDE and EXECUTE,
  capped fills, stale write-off, exit-rate decay and snap, locate-in-aim before neutralisation, the fixed-rate
  liquidity cache and nav_summ post-ramp (recomputed .78340).
- **Not found:** no look-ahead, UB or cross-lane semantic defect. RAM and time fit 180 s / 1536 MiB; the tightest
  phase is the cold u pass at 122 s.

**I1 (Important): "D2 for C2" had no artifact behind it.** The root re-ran the loc cell on the v6-2 exe
(`...-loc-v62`, exe `f55537fc`, 34.1 s, 340 MiB) and compared it with the v6-0 cell (`212d9e22`). This lane
re-checked the result independently with sha256:

| file | result | sha256 (first 16) |
|---|---|---|
| `daily_linear-6bps-stale5-v1+swap-fin-v1.csv` | SAME | `68330d7aafce01f1` |
| `daily_modeled-1bn-stale5-v1+engine-tiers-v1.csv` | SAME | `c55e6b42ebb72ccf` |
| `daily_modeled-1bn-stale5-v1+flat-300-v0.csv` | SAME | `cbe42086ddec7234` |
| `daily_modeled-1bn-stale5-v1+swap-fin-v1.csv` | SAME | `493cc6c2254f07d0` |
| `daily_modeled-1bn-terminal-adverse-v1+swap-fin-v1.csv` | SAME | `3ac6150cf205c2ca` |
| `events_linear-6bps-stale5-v1+swap-fin-v1.csv` | SAME | `1e0e00e97f68676f` |
| `events_modeled-1bn-stale5-v1+engine-tiers-v1.csv` | SAME | `315ae9964292005d` |
| `events_modeled-1bn-stale5-v1+flat-300-v0.csv` | SAME | `14b7bf90e62cb138` |
| `events_modeled-1bn-stale5-v1+swap-fin-v1.csv` | SAME | `8ec3b0f29f5e88a3` |
| `events_modeled-1bn-terminal-adverse-v1+swap-fin-v1.csv` | SAME | `ceb1a8eb1f40579c` |
| `recipe.json` | DIFF, as allowed | key `exit_rule` renamed `exit_rate_rule` (same text); `aim_partial` clause "nonmembers exit to 0" became "nonmembers follow exit_rate_rule" |
| `summary.json` | DIFF, as allowed | only `recipe_sha256` (`f1f80cc6...` -> `1ed6fa41...`) |

So I1 is **CLOSED**. The C2 default price-risk-v1 path is byte-stable on real TRAIN data, and cells 11-15, which
ran on v6-2, are not confounded by the binary.

**m1 (IC binary for V6-U): CLOSED.** The v6l ew weighted pass was re-run on IC exe `b1c1ba07`
(`mega-v6lw-train-ew-2`, 41.5 s, 505 MiB) and compared with `647c71a7` (`-1`, 45.5 s):
- All 6 `train_combined.*` files are SAME: `.f64` `545922d2...`, `.json` `2b4a1af3...`, `_finite.u8`,
  `_member.u8`, `_ids.u64` and `_sessions.i64`.
- `orientations.json`, `recipe.json`, `train_daily_ic.csv` and `train_planned_targets.csv` are also SAME.
- `summary.json` and `train_candidates.jsonl` differ only in `wall_seconds` / `stage_seconds` timing fields.
- So the W runner change is a no-op for schema-v1 weights.

**Resulting status: MERGE-READY (owner gate U5).**

Remaining minors:
- m2: `v6l_train.sh` silently falls back to the wrong REFN. Mitigated: REFN and DSR_N were passed explicitly for
  V6-U / V6-F.
- m3: the `summary.json` limitations text is stale under delta orders.
- m4: the scripts do not check the receipt `exit_code`.
- m5: `role --max-seconds` defaults to 300 (v6u_train.sh passes 170).
- m6: V6-U changes `mkt_ret`.
- m7: the byte-identity claims need `-ffp-contract=off` before any AVX2 build reuses them.
- m8: no gtest covers the full stack.
- m9: closed by the V6-U ruling, declared before the read: L = round(1/.8019, 3), one cell, gate band
  [.90, 1.05].

**Disclosures the handoff carries (D1-D15, in substance):**
- **D1 Locate-in-aim rescale.** Gross is rescaled to the zeroed entry gross (1 minus about 100 zeroed special-short
  names per decision), not to 1. After price-risk-v1 a zeroed name keeps `-fitted * scale`, which can be long. The
  shared construction means flat-300 and engine-tiers also lose those shorts, and swap-fin's post-block remains the
  safety net.
- **D2 Recipe key rename mid-grid.** The 10 v6-0 cells carry `exit_rule` and a stale "nonmembers exit to 0" clause;
  the v6-2 cells carry `exit_rate_rule`. Statistics are unaffected (I1 above proves it for the loc cell).
- **D3 Admit estimate 2304 MiB.** This is the IC runner admission envelope for ew-theme-v6 theme planes. The runner
  RSS cap stayed 1536, and the real peak was 1199-1257 MiB.
- **D4 hold_zero approximations (ind ids; code merged, cells rejected).**
  - The held set is a superset of "zeroed".
  - Group neutrality is approximate with holds.
  - Held names can be shorted slightly in unblocked books.
  - The fallback pool can hold fewer than 5 names.
- **D5 Golden (a) is a Python-replica golden.** Base equality of price-risk-v1 rests on the I1 A/B, which now
  exists.
- **D6 Paths not exercised on real data.** With cadence 1 and no skipped rebalance, delta residual carry-over beyond
  one session and the kept-order delta-to-target conversion are tested in gtests only.
- **D7 Stale limitations text.** Under the delta basis, the `summary.json` limitations text still says
  "decision-NAV dollar targets" (m3).
- **D8 Absent nonmember re-decay.** An absent nonmember gets an immediate exit. If it reappears before the fill, the
  next decision replaces the exit with a decay.
- **D9 Exit decay lingers.** Exited names stay about 58-63 sessions: held share 1.04 in the final cell, and more
  unhedged nonmember exposure.
- **D10 L policy.** Derived from post-ramp gross, declared single-shot (final cell L 1.247 from .8019).
- **D11 DSR scope.** N counts NAV cells only, and uses the Lo variance for single-dir runs. Acceptance was sign-only
  and within noise.
- **D12 Library v6 proxies.**
  - res_mom is a standardized in-window CAPM alpha vs the equal-weight member market, not the BHM FF3 residual. It
    is a re-trial of v4.2.
  - cbop relies on `pow(NaN,0) = 1` and is a cash-flow-statement approximation.
  - value_composite uses 3 of 4 ratios.
  - smax uses MAX1, not MAX5.
  - bac has no vol-quintile conditioning.
  - ind_mom / within_ind_mom keep the 21-session blackout.
  - The "fast" sets differ between L (v5.1 taus) and W (v6 taus).
- **D13 V6-W (rejected; code merged).**
  - `signal_semantics` is unchanged for a themed blend; `composition_redistribution` is the marker.
  - Per-name redistribution undoes the (c) shrink where slow members are missing.
  - reversal_seasonality stays unshrunk under literal (c).
- **D14 V6-U.**
  - The rehearsal bridge is `scope_complete false`, so unbridged operating stocks drop. ADRs of linked filers stay.
    REITs stay. Royalty trusts 6792/6795 are excluded.
  - `membership_recipe` still names the ADV top-N rule.
  - `mkt_ret` and group ranks are recomputed over the restricted members.
  - Admission was re-run. So V6-U = universe + market proxy + admission set.
- **D15 Binary provenance.**
  - Cells 1-10 and D2: NAV `212d9e22` (v6-0).
  - Cells 11-15 and I1: NAV `f55537fc` (v6-2).
  - Role-v2 combined: IC `647c71a7`.
  - V6-U u / w and ew6 w: IC `b1c1ba07` (v6-1). Its build receipt records 1 dirty entry; m1 shows identical bytes
    on the unthemed path.

## 6. Owner decision packet

**Gate read-out (ledger, declared rule, not softened):** the goal is reached on TRAIN (S2 net +1.182, mechanics
pass), but the pre-registered DSR gate is not met (.904 < .95). **The controller makes NO freeze proposal.** The owner
decides whether validation trial #3 is spent on this single cell with DSR .904 disclosed.

| Gate | Decision asked | Recommendation / facts |
|---|---|---|
| **U1** | Spend validation trial #3 on the single final cell (2023-2024, book level only), or not | Owner call; no controller freeze proposal. See the facts and the read-out protocol below the table. |
| **U5** | Merge the integration branch into main (a real merge; main has diverged) | **Ready.** 0 Critical and 0 open Important after I1 and m1; carry D1-D15 and the minors in the merge description. Handoff 4 made U5 conditional on a clean whole-branch verdict, and that condition is now met for v6. |
| U2 | Pre-2020 history for selection and estimation (carried from handoff 4) | Still the most direct fix for the statistical problem. On 3 years every v6 step had \|t\| < 1, and the DSR gate fails at N 28. A longer TRAIN is the only way to shrink both. |
| U3 | New data (carried, extended) | (a) bridge / CIK coverage: V6-U, the largest single step, rests on the r4 rehearsal bridge (`scope_complete false`), and delisting returns (S3) need the same coverage. (b) Fields the v6 library could not express (V6-L §3): Heston-Sadka lags 24/36, FINRA daily short-sale volume, SG&A / intangible capital, 5-year issuance, XFIN (6 extras), MAX5 (`ts_topk_mean`). |
| U4 | Cost target (S1 vs S2) and AUM (carried) | At the final cell S1 is +1.324 and S2 +1.182. Cost per $ rises with gross: 12.82 -> 13.55 bps from L 1 to L 1.247. At $1bn the objective still depends on the cost model. |

**Facts for U1:**
- For a single cell: TRAIN net +1.182, HAC t 2.08, cross-cell DSR .904 and Lo DSR .499.
- End to end vs v5 REF: t 1.73.
- 2020 contributes ~0.
- 2023-2024 has been read twice before, for trials #1 and #2 at book level. Nothing in v6 used it.
- Two years of VAL gives SE(SR) ≈ sqrt((1 + 1.18²/2)/2) ≈ .92. VAL can falsify (for example, net <= 0) but cannot
  confirm SR >= 1.

**Configuration a U1 grant would freeze** (for reference only, not a proposal; declare it in the ledger as
"FREEZE v6-final-2026-09-28 - VALIDATION TRIAL #3" before any 2023+ read). It is every pin in §3, plus:
- universe rule `linked-operating-v1` with the same bridge (`ddf97164`) and fundamental-events (`74ed9a50`) pins
  and the same non-operating SIC set;
- fields list = lo1-fields-v6b (40 fields, fields-v6 recipe, fund lag 1 session);
- orientations `84d72e6c` and weights `490c3836`, both TRAIN-fitted and not re-fitted;
- NAV flags exactly as §3 with **L 1.247 fixed**, not re-derived on VAL;
- IC exe `b1c1ba07` and NAV exe `f55537fc`.

**Recommended read-out protocol if U1 is granted** (declare it verbatim in the ledger before step 1; one run; no
changes afterwards):
1. **Data (part of trial #3, no statistic):**
   - Restrict the existing VAL role `recent-fast-validation-2023-2024-v1` (manifest `0c757c41...`, used by trial
     #2) with `prepare_recent_research.py role --universe linked-operating-v1`. Use the same bridge and events pins,
     `--check-fields` on the VAL fields-v6, and `--max-seconds 170`.
   - Gate on the restriction manifest (metadata only): min kept members on scored days >= 1000; report the dropped
     share and reasons.
   - Rebuild the VAL fields with the lo1-fields-v6b list on the restricted VAL role. `mkt_ret` becomes the
     restricted-member market, as on TRAIN.
2. **IC runner, once** (pattern: `studies/v4_validation_once.sh`):
   - `--train` lo1 role, `--validation` restricted VAL role, `--validation-fields`.
   - `--orientations` = the v6u u-5 file; `--composition-weights` = `490c3836`.
   - Candidate cache `mega-candidate-cache-v6u`; `--save-combined`.
   - **Never open the runner's per-candidate validation rows** (`summary.json` candidate sections, daily IC CSV).
     Use only the combined file's sha.
3. **NAV, once, on the restricted VAL role** with the §3 flags and L 1.247.
4. **Read-out, book level only:** `nav_summ.py` single dir. Report:
   - S2 net SR, gross SR, HAC t;
   - all-rows and post-ramp gross, mean net, tau;
   - cost bps/$;
   - S1 / tiers / flat-300 / S3;
   - 2023 and 2024 net returns.

   No per-candidate or per-theme VAL statistic. Nothing is tuned afterwards. The result is recorded as-is.
5. **Pre-declared interpretation (proposal; the owner may amend it before step 1):**
   - Primary = VAL S2 net SR.
   - "Consistent" if VAL S2 net > 0 and R6' mechanics hold on VAL all rows.
   - "Fail" otherwise.
   - Report the VAL SR with its Lo SE and the TRAIN-VAL gap. A VAL net >= 1.0 is not required, and by itself would
     not establish SR >= 1.
   - Note: the deployment ramp (~63 sessions from 0) is ~12% of a 2-year window vs 8% of TRAIN, so VAL all-rows
     gross and SR are mechanically diluted. Report post-ramp alongside.

If U1 is not granted: record the cell as "TRAIN objective met, DSR gate not met, not validated" and stop. The ruling
"no further cells to move the DSR" stands.

## 7. Open items / parked findings

- **Pre-registered but not run (no statistic, not trials):**
  - ew-theme-v6 on library v5.1 (the V6-W budget named v5.1 first). Only the library-v6 variant ran.
  - loc x dust .2 (ruling: +0.007 inside noise).
- **2020 contribution ≈ 0 in the V6-U cells.** The universe restriction and C5 both removed the 2020 alpha that
  role v2 had (+2.8% in the loc cell). This is unexplained. A disclosed diagnostic, for example a by-bucket
  aggregation of existing TRAIN outputs as in v6-explore, could say which names carried 2020. It must not feed a new
  lever without a new prereg.
- **Branch review minors m2-m8** and the per-task minors (V6-L 7, V6-C1 M1-M7, V6-C2 M1+, V6-W 10) are open by
  design. They are optional polish, disclosed as D1-D15.
- **v6-1 build receipt shows `DirtyEntries 1`** (source `a48677cb`). It is not identified in the ledger. The m1
  byte-identity bounds its effect on the unthemed IC path; the themed path (ew6) is rejected.
- **nav_summ n28 `netting_ratio`** is computed with the v6u weights for every cell, so it is UNMATCHED for 26 of 28
  cells. It is cosmetic, and the scorecard omits the column.
- **Held share > 1** (1.04 final cell): the exit decay lingers ~60 sessions (D9). Nonmember exposure is unhedged by
  the member OLS.
- **Carried from handoff 4:**
  - delisting lane T33b/T32 parked (U3);
  - `per_name_rates` cache read parked (`check_rates` fails loudly);
  - nav_summ single-dir warning noise;
  - aim L overshoot;
  - a gross-targeting rule (L solved per session) as the principled replacement for the deterministic L (a new
    construction family, new prereg);
  - 28 acceptable deferred minors from T41 triage.

## 8. Rulings made in the v6 session (every `Ruling:` line of the v6 ledger sections, oldest first)

1. v6 is a new disclosed revision. Every v6 lever set is pre-registered before any v6 TRAIN read. The mechanics gate
   keeps gross ~1, and under-deployment as a route to net >= 1 is forgone deliberately.
2. The v6-explore aggregation of existing TRAIN NAV outputs is a disclosed diagnostic study, not a strategy trial.
3. Phase A reviewers may read every existing TRAIN artefact as a disclosed diagnostic. None may read 2023+ data or
   per-candidate VAL statistics.
4. "Optimal combination" means a trade-cost-aware combination fitted on TRAIN with a pre-registered method and
   shrinkage. No per-candidate weight search against TRAIN net SR without DSR accounting.
5. Phase C runs four implementers in parallel in disjoint pools with declared file ownership (C1 pool-10, C2
   pool-11, W pool-4, L pool-5). The root cherry-picks and resolves.
6. Acceptance of every v6 step is the SIGN of the paired dSR vs its parent matching the pre-registered prior, not its
   magnitude, because SE(SR) over 3 years is ~.63.
7. Lit lever R (name-level borrow fees) is already represented by swap-fin-v1's GC/warm/special tiers, so S2 is
   unchanged. Disclosed: modeled net SR may be overstated for hard-to-borrow shorts.
8. The extra-field cap stays at 5, so XFIN is not added (cap 6 ≈ 1431 MiB; the RAM rule is efficiency, not longer
   caps).
9. Additions are placed before incumbents in roster order, so redundancy keeps the upgraded definition (the "replace"
   intent). This is a design choice, not a data-driven one.
10. Merge order: C1 first, then a child rebases C2 onto pool-2 HEAD in pool-11.
11. Build tag v6-0 on C1 runs in parallel with the C1 review. "May not compile" is only testable by the root build.
12. EXIT_RATE < 1 requires dust > 0, so the dust re-tune set becomes {.05, .2} instead of {0, .2}. The trial count
    is the same.
13. Locate-in-aim applies to every scenario book, because the construction is shared. Disclosed: flat-300 loses a
    few shorts.
14. The bounded runner RSS cap stays 1536 MiB for every phase. The fitter's 2304 is an admit estimate.
15. reversal_seasonality stays unshrunk under literal rule (c). The prereg text is binding.
16. V6-U needs a fields rebuild for the restricted role (a root data step, disclosed, not a trial).
17. The remaining 5 C1 grid cells wait for the C1 review verdict, since review-forced re-runs would still count in
    DSR N.
18. C3 and C5 are evaluated as separate cells. Combining them needs the re-zero-after-demean fix first.
19. The C2 rebase waits for the C1 review verdict and fix wave.
20. Library v6 uses its own candidate cache (`mega-candidate-cache-v6`). The runner's refusal was correct, and
    failed runs are not trials.
21. Declared before any grid read: the V6-C grid runs with PARENT = the V6-L cell (library v6), not the v5.1 parent.
    The 6-cell budget is unchanged.
22. Royalty trusts (SIC 6792/6795) join the non-operating exclusion set. REITs stay (literal prereg).
23. The 5 remaining grid cells run now on the v6-0 binary. The only C1 defect is in a test, and the binary is the one
    that produced the byte-identical D2 cell.
24. The grid runs through `studies/v6l_train.sh` (knobs ORDER_BASIS / EXIT_RATE / LOCATE_AIM / LCACHE / THETA /
    DUST / LEV / NEUT), not `v6_train.sh`.
25. Conditional cells run on the best grid cell (delta, x.05): C3 loc; theta .03 (exit .05 kept); dust .05 and .2.
    That is 4 cells, DSR N 20-23.
26. The 10 v6-0 cells stand although the recipe key `exit_rule` was renamed `exit_rate_rule`. Statistics are
    unaffected, and the final cell is re-run on the final binary.
27. Two build tags: v6-1 (IC) now, v6-2 (NAV) after the C2 rebase.
28. C2 concerns are accepted as disclosures: the hold set may include already-zero special-tier names, and held
    names keep a small fit term.
29. Declared before the final read: V6-F = the V6-U stack with L = 1/gross_lev_post_ramp = 1.247 and dust .1. dust .2
    x loc is not run (+0.007 on SE .12). The gate is all-rows gross [.90, 1.05], |mean net| <= .02, tau limits,
    S2 net >= 1.0, then the cross-cell DSR over N = 13 + v6 cells.
30. No further cells are run to move the DSR: every added TRAIN cell raises N. The pre-registered condition is
    binding, and the alternative is p-hacking.

## 9. Goal prompt for the next controller

```
/goal Resume mega-alpha after the v6 sprint. Read, in order: docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md (this
handoff), docs/plans/2026-09-28-mega-alpha-scorecard-v6.md, .superpowers/sdd/mega-alpha-20260926/task-V6-branch-review.md,
and the top sections of .superpowers/sdd/mega-alpha-20260926/progress.md (newest first; "GATE READ-OUT" is the latest).
Trust the ledger and git log over memory.

State: v6 closed on TRAIN. The final cell build-equity/mega-nav-v6u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247
(library v6 5ee66d13 x ew-theme-v1 490c3836 on the linked-operating-v1 role 3e79978a; aim-partial-v5 theta .05 dust .1,
delta orders, exit rate .05, locate-in-aim, L 1.247; NAV exe f55537fc, IC exe b1c1ba07) has S2 net +1.182 at gross .971
with R6' mechanics PASS, but cross-cell DSR .904 < the pre-registered .95 (Lo .499; N 28). No freeze was proposed.
Validation trial #3 is unspent. Branch: MERGE-READY (I1/m1 closed by byte-identity re-runs); merge = owner gate U5.

First: ask the owner which of U1-U5 (handoff 5 section 6) are answered. Then:
- If U1 is GRANTED for this single cell: append to the ledger, BEFORE any 2023+ read, "FREEZE v6-final-2026-09-28 -
  VALIDATION TRIAL #3" with every pin of handoff 5 section 3, the DSR .904 disclosure, and the read-out protocol and
  interpretation rule of handoff 5 section 6 (owner amendments, if any, verbatim). Then execute steps 1-4 exactly once:
  restrict the VAL role with linked-operating-v1 (same bridge / events pins), rebuild the VAL fields with the
  lo1-fields-v6b list, one IC runner validation pass with the frozen orientations and weights (never open per-candidate
  VAL rows), one NAV run with the final-cell flags and L 1.247, nav_summ single dir. Book level only. Record the result
  whatever it is; no re-run, no tuning; write handoff 6.
- If U1 is NOT granted: there is nothing to tune. Do not add levers, cells, compositions or libraries on TRAIN
  2020-2022 (every new cell raises N and the prereg DSR condition is binding). Only owner-granted work proceeds: U5
  merge (carry D1-D15), U2 (extend TRAIN before 2020: new prereg section in v4-prereg.md before any read; re-measure
  the frozen final-cell recipe on the longer TRAIN), U3 data asks, U4 cost/AUM (pre-register as a new disclosed
  revision before any read).

Method: subagent-driven development; Opus 5.5 for every implementer, explorer, researcher and reviewer; artifacts as
files; children get their brief path, plan-s3-constraints.md, their Interfaces block and lane-contract.md; reviewers get
review-TN.diff + reviewer-contract.md (re-reviews: re-review-contract.md); <= 5 fix rounds; controller never implements.
Pools: 2 = root; children create their own branch in their pool (the controller cannot switch pool branches); never 1 or 6.

Rules (verbatim, binding): work only in C:/atx-wt/pool-2 (feat/aes-codex-integration-20260925); never mutate/build/switch/commit
in C:/atx (read-only ok; the tier1-v2 session owns C:/atx and atx-db: no locks, never kill its processes). Root alone builds
via powershell -File build-equity/mega-build.ps1 -Tag <new> -Targets "<t1,t2>" (tags v5-0, v5-1, v5-2, v6-0, v6-1, v6-2 used;
check build-equity/mega-<tag>-receipt.json does not exist). Root alone runs real data under
"C:/Program Files/Python312/python.exe" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512
--output build-equity/<new-dir> --bind <manifests> -- <cmd>; tree must be clean (untracked included): commit docs, briefs,
ledger and scripts (git add -f for the gitignored sprint dir) before every run. Children: own pool worktree, never build,
never run real data, never spawn subagents, Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com> trailer, task-TN-report.md,
reply < 15 lines. TRAIN 2020-2022 only unless U2 extends it; 2023-2024 used twice (any new read = disclosed validation trial #3,
needs owner gate U1); 2025+ reserved; never read a per-candidate VAL statistic. No pushes, warehouse writes or broker actions.
No TDD; postimplementation fixtures; tasks accepted on root real-data measurements. RAM: limits stay 180 s / 1536 MiB,
efficiency fixes not longer caps. Every ruling: "Ruling: <decision> -- <why> -- cost if wrong: ...", declared before any
measurement it could bias; report every TRAIN (and the one VAL) result with the Appendix A trial-accounting block.
```
