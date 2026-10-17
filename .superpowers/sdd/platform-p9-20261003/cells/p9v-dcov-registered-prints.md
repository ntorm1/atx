# P9-V B.8(c)-(e), existing registered D-COV prints

Copied exactly from the prereg B.8 tables at this checkpoint; no model or statistic recomputation.
Every row read in both directions: below band over-forecast/over-corrected, above band under-forecast. These lines decide nothing.

  - **(c)** D-COV's families, v1.1 | atx-cov-v1 (`bias_summary.json`):

| family | status | series ok | obs | full b mean | full b median | in band | in kurt. band | roll63 share / b mean | roll252 share / b mean | QLIKE | MRAD 63 / 252 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| random | ok / ok | 64 / 64 | 69,696 | 0.9603 / 0.9611 | 0.9618 / 0.9624 | 34 / 34 | 44 / 44 | .7232 / .9431 \| .7234 / .9438 | .6072 / .9369 \| .6105 / .9374 | 2.4080 / 2.4078 | .1321 / .0824 \| .1320 / .0820 |
| factor | ok / ok | 51 / 51 | 51,193 | 1.0016 / 1.0041 | 1.0050 / 1.0181 | 27 / 25 | 36 / 35 | .6361 / .9745 \| .6364 / .9760 | .5764 / .9803 \| .5809 / .9812 | 2.5149 / 2.5144 | .1693 / .0944 \| .1692 / .0949 |
| eigen | ok / ok | 51 / 51 | 54,770 | 434.61 / 379.78 | 1.1629 / 1.1217 | 10 / 9 | 48 / 48 | .4346 / 367.78 \| .4824 / 331.78 | .2568 / 430.53 \| .2849 / 379.74 | 5,621,299.8 / 4,149,159.8 | 361.50 / 421.87 \| 326.03 / 371.99 |
| minvar | ok / ok | 1 / 1 | 1,089 | 1.4995 / 1.4747 | 1.4995 / 1.4747 | 0 / 0 | 0 / 0 | .5063 / 1.3364 \| .5628 / 1.3110 | .1969 / 1.3145 \| .2792 / 1.2908 | 3.1942 / 3.1430 | .3725 / .3178 \| .3577 / .2974 |
| optimized | ok / ok | 32 / 32 | 34,848 | 1.0081 / 1.0078 | 1.0045 / 1.0044 | 23 / 23 | 26 / 27 | .7409 / .9921 \| .7408 / .9918 | .7692 / .9900 \| .7694 / .9898 | 2.4311 / 2.4271 | .1341 / .0602 \| .1341 / .0601 |

  - **(d)** D-COV's eigen family per rank (b = sample SD of z, ddof 1; T = 1,089 for ranks 00-47, 948 / 817 / 733 for
    ranks 48 / 49 / 50; band sqrt(2 / T) = .0429 (.0459 / .0495 / .0522); reduction = |b_v1.1 - 1| - |b_cov - 1|,
    positive = atx-cov-v1 nearer 1):

| rank | b v1.1 | b atx-cov-v1 | v1.1 | atx-cov-v1 | reduction |
|---|---|---|---|---|---|
| 00 | 16074.6219 | 13667.7331 | above | above | +2406.8888 |
| 01 | 4802.5569 | 4565.5452 | above | above | +237.0117 |
| 02 | 1230.4744 | 1079.8917 | above | above | +150.5827 |
| 03 | 1.8387 | 1.6799 | above | above | +0.1588 |
| 04 | 1.6923 | 1.5373 | above | above | +0.1550 |
| 05 | 1.7420 | 1.6582 | above | above | +0.0838 |
| 06 | 1.6976 | 1.5493 | above | above | +0.1483 |
| 07 | 1.6226 | 1.5702 | above | above | +0.0525 |
| 08 | 1.5256 | 1.4314 | above | above | +0.0942 |
| 09 | 1.6104 | 1.4994 | above | above | +0.1110 |
| 10 | 1.5563 | 1.4260 | above | above | +0.1303 |
| 11 | 1.5363 | 1.3884 | above | above | +0.1480 |
| 12 | 1.3838 | 1.3174 | above | above | +0.0664 |
| 13 | 1.3362 | 1.2963 | above | above | +0.0399 |
| 14 | 1.3258 | 1.2274 | above | above | +0.0984 |
| 15 | 1.3030 | 1.2265 | above | above | +0.0765 |
| 16 | 1.3352 | 1.2610 | above | above | +0.0742 |
| 17 | 1.3218 | 1.2462 | above | above | +0.0756 |
| 18 | 1.3773 | 1.2865 | above | above | +0.0908 |
| 19 | 1.3108 | 1.2384 | above | above | +0.0724 |
| 20 | 1.3225 | 1.2681 | above | above | +0.0544 |
| 21 | 1.2136 | 1.1690 | above | above | +0.0446 |
| 22 | 1.2481 | 1.2251 | above | above | +0.0230 |
| 23 | 1.2400 | 1.1790 | above | above | +0.0610 |
| 24 | 1.1761 | 1.1868 | above | above | -0.0107 |
| 25 | 1.1629 | 1.1150 | above | above | +0.0479 |
| 26 | 1.1523 | 1.1217 | above | above | +0.0306 |
| 27 | 1.1432 | 1.1135 | above | above | +0.0297 |
| 28 | 1.1202 | 1.0983 | above | above | +0.0219 |
| 29 | 1.1196 | 1.1035 | above | above | +0.0161 |
| 30 | 1.0858 | 1.0993 | above | above | -0.0136 |
| 31 | 1.0962 | 1.0598 | above | above | +0.0364 |
| 32 | 0.9869 | 0.9337 | in band | below | -0.0532 |
| 33 | 0.8009 | 0.8837 | below | below | +0.0828 |
| 34 | 1.0167 | 1.0515 | in band | above | -0.0349 |
| 35 | 1.0781 | 1.0883 | above | above | -0.0102 |
| 36 | 1.0251 | 1.0255 | in band | in band | -0.0004 |
| 37 | 1.0280 | 1.0220 | in band | in band | +0.0060 |
| 38 | 1.0393 | 1.0761 | in band | above | -0.0368 |
| 39 | 0.9842 | 0.9783 | in band | in band | -0.0060 |
| 40 | 1.0003 | 1.0222 | in band | in band | -0.0220 |
| 41 | 0.9882 | 1.0003 | in band | in band | +0.0115 |
| 42 | 1.0015 | 1.0261 | in band | in band | -0.0245 |
| 43 | 0.9165 | 0.9630 | below | in band | +0.0465 |
| 44 | 0.9863 | 0.9694 | in band | in band | -0.0169 |
| 45 | 0.9437 | 1.0023 | below | in band | +0.0540 |
| 46 | 0.9113 | 0.9402 | below | below | +0.0289 |
| 47 | 0.7836 | 0.8009 | below | below | +0.0172 |
| 48 | 0.7365 | 0.7536 | below | below | +0.0171 |
| 49 | 0.7118 | 0.7521 | below | below | +0.0403 |
| 50 | 0.8135 | 0.8004 | below | below | -0.0131 |

  - Read in both directions (COV-11; gate report wording): ranks in band v1.1 10 of 51, atx-cov-v1 9 of 51; ranks
    reduced 39 of 51 (12 negative); below band = over-forecast / over-corrected in v1.1 at ranks 33, 43, 45-50 (8 ranks)
    and in atx-cov-v1 at ranks 32, 33, 46-50 (7 ranks). Ranks 00-02 have b above 1,000 in both recipes, so every pooled
    and mean eigen figure is dominated by them (root's finding, ledger :582; COV-13).
  - **(e) The three pooled eigen rows (COV-13 (b)):**

| pooled eigen row | N | band | b v1.1 | b atx-cov-v1 | source |
|---|---|---|---|---|---|
| all ranks | 54,770 | .0060 | 2370.9495 | 2036.9591 (both above) | D-COV, gate report item 4 (d) |
| without eigen-00 | 53,681 | .0061 | 705.8525 | 667.9782 (both above) | D-COV, same |
| without structurally null ranks | 54,770 | .0060 | 2370.9495 | 2036.9591 (= the all-ranks row: no rank excluded) | D-COV-EIG outcome as COV-14 (ledger :621) rules it |

  - Reading rule (COV-13 (b), fixed before D-COV-EIG runs): a rank is "structurally null" iff its median
    squared-loading share on (i) factors flagged structural plus (ii) the industry block and the country / market factor
    is at least .99; such a rank is excluded from the third row; a rank that fails the rule stays in every pooled row and
    is printed as a finding. COV-13 had P9-V's registered predictions quote the third row, with the all-ranks row printed
    beside it.
  - **D-COV-EIG's outcome (COV-14, ledger :621).** The reading rule stands as fixed before the look. No rank 00-02 is
    structurally null in either recipe, so no rank is excluded and the third pooled row equals the all-ranks row; the
    three ranks are a FINDING (near-null factor directions under-forecast by a factor above 1,000 in both recipes).
    P9-V's registered predictions quote the all-ranks pooled row and the per-rank table (d). The pooled eigen figure is
    dominated by ranks 00-01; printed beside it: the count of ranks nearer 1 under `atx-cov-v1`, 39 of 51 ((d), column
    "reduction"). No exclusion rule is added and no `atx-cov-v1` parameter moves (COV-5); COV-13 (c) binds COV-MV
    unchanged (B.5). The model defect is a P10 item for the risk model.
