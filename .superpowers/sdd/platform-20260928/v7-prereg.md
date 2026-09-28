# v7 pre-registration (platform-20260928) -- declared 2026-09-28 before any L4 measurement

Baseline: v6.1 final cell mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247 (S2 net +1.239, TRAIN 2020-2022,
cross-cell N = 29). Statistics protocol: v4-prereg.md unchanged (paired dSR with Memmel SE, CBB, LW; cross-cell DSR and
Lo single-cell; R6' mechanics gate gross in [.90, 1.05], |net| <= .02, tau mean <= .20 / p95 <= .30; freeze needs S2 net
>= 1.0 AND mechanics AND DSR >= .95).

## Descriptive (no trial)
- Risk model atx-risk-v1 exposures/covariances and the bias harness on random, factor-mimicking and the v6.1 book
  portfolios. Reported, not used for acceptance.
- Cost scenarios S2-KO and S2-FIM on the v6.1 cell's planned trades; capacity curve at NAV multiples {.5, 1, 2, 4, 8}x.
  S2 stays primary. No cell is chosen on these.
- Release-build identity re-runs and research_cycle.py reproduction runs: identity checks, not trials.

## Construction trials: aim-partial-v6 (R2.2 + R2.3), library v6.1, ew-theme-v1, L 1.247 fixed, same flags as baseline
Grid of exactly three cells (cross-cell N 29 -> 32), parameters from the literature, none tuned on TRAIN:
| cell | kappa (cost shrink) | band b | rate clip |
|---|---|---|---|
| C1 | 0.5 | dust-equivalent (median band = dust .1) | [.5, 1.5] |
| C2 | 1.0 | dust-equivalent | [.5, 1.5] |
| C3 | 1.0 | dust-equivalent | [1, 1] (no regime rate) |
Acceptance of a cell over the baseline: paired S2 net dSR > 0 (sign-only inside one SE, as in v6) AND mechanics AND
cost per dollar not higher; PBO over {baseline, C1, C2, C3} reported (CSCV, 16 blocks) and a winner is accepted only if
PBO <= .2. Effective-N DSR (ONC clusters over the trial ledger) reported beside the cell-count DSR from v7 on; the v6
gate is not re-scored.

## Identity requirement
aim-partial-v6 with kappa 0, band = dust, clip [1, 1] must reproduce aim-partial-v5 bit-for-bit on the v6.1 cell before
any C1-C3 cell is run.

## Trial accounting (Appendix A) after this grid
TRAIN construction cells: 29 + 3 = 32. Validation trials: 2 spent (unchanged). 2025+: reserved.
