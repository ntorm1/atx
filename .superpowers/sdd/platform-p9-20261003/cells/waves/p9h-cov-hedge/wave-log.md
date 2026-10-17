### Cell p9h-cov-hedge (rule wave; template `theme-cov-hedge-ready.json` on v8ysb): N 64

**ACCEPTED.** Manifest `scripts/specs/p9/waves/p9h-cov-hedge-attempt2.json` sha256 `c4d682c33fb216c2` (commit `708ab6ceac4e`); parent `scripts/specs/p9/templates/label-terminal-gm.json` (library v8ysb, L 1.1828); driver `research_cycle.py wave run`.

Budget p9-prereg-construction: construction N 63 -> 64 of 71.

**Cell** `scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json` (rule, library v8ysb): gross match pm6-6-engine: calibration L 1.1828 G 0.9992581911 vs G_parent 0.9862134133 -> corrected to L 1.1674, G 0.9862418937.

**Mechanics (S2, read before any return): PASS** (gross_all_rows 0.98624 [0.9, 1.05]; abs_net_all_rows 0.00507 <= 0.02; tau_mean 0.027062 <= 0.2; tau_p95 0.032348 <= 0.3; max_return_identity_error 3.7817e-16 <= 1e-09; max_cash_book_relative_error 1.2565e-13 <= 1e-09).

**Statistics of record** (S2): net Sharpe 1.8750 vs parent 1.8495: dSR +0.0255, Memmel SE 0.2615, CBB 95% [-0.49947667984762295, 0.5797562683890513], LW p 0.9322; bundle p one-sided 0.4744, two-sided 0.9322. DSR (N 64): ledger 0.8446; PBO 0.1291.

**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): ACCEPTED** {'criteria': True, 'dsr_positive': True, 'mechanics': True}.
- criterion capacity-4x-higher (printed): met
- criterion turnover-per-gross-not-higher (printed): met
- criterion cost-bps-lower (printed): met

Returns (S2, annual): net 5.69% (CAGR 5.81%) vs 5.65%; gross of cost 7.06% vs 7.07%; vol 3.03%; max drawdown 2.20%; 4x net Sharpe 1.7596 vs 1.6994; tau 0.02706 (per unit gross 0.02744).

Ledger `build-equity/trials.jsonl`: lines 134 -> 135, head `1568e4a9ef832de9`, N 63 -> 64, cell trial `94a77d4af05ac735`; admission lines appended 0.

| phase | run dir | s | peak MiB | outcome |
|---|---|---|---|---|
| u | `build-equity/mega-p9l-train-u-label-terminal-run1` | 51.6 | 933 | completed |
| fit | `build-equity/mega-weights-p9-theme-cov-hedge-run1` | 3.1 | 66 | completed |
| w | `build-equity/mega-p9w-train-theme-cov-hedge-run1` | 35.6 | 1757 | completed |
| card | `build-equity/mega-cards-p9-theme-cov-hedge-run` | 24.8 | 1441 | completed |
| nav | `build-equity/mega-nav-p9-theme-cov-hedge-gm-run` | 52.9 | 693 | completed |
| monitor | `build-equity/mega-monitor-p9-theme-cov-hedge-run` | 2.7 | 133 | completed |
| summ | `build-equity/cycle-theme-cov-hedge-v1-attempt2-gm/summ-run1` | 13.6 | 69 | completed |
| reader:book | `build-equity/waves/p9h-cov-hedge-attempt2/readers/book-run1` | 0.8 | 40 | completed |
| reader:mech-matched | `build-equity/waves/p9h-cov-hedge-attempt2/readers/mech-matched-run1` | 0.5 | 32 | completed |
| reader:mech-parent | `build-equity/waves/p9h-cov-hedge-attempt2/readers/mech-parent-run1` | 0.8 | 43 | completed |
| bundle | `build-equity/waves/p9h-cov-hedge-attempt2/bundle-run1` | 0.8 | 50 | completed |

Stage seconds: preflight 0.9, register 0.0, screen 0.0, spec 1.4, run 67.8, match 59.4, verify 0.2, judge 22.6.

Hidden-data record: seal scan of 40 log(s) (every run dir, reader and console of the wave; forms iso, compact, year, quarter): 0 date token(s) at or after 2024-01-01 (2026-10-02 x7 allowed: R15-SEAL-D; 2026-10-03 x21 allowed: R15-SEAL-D; 2026-10-06 x7 allowed: R15-SEAL-D; 2026-10-17 x4 allowed: R15-SEAL-D; 20260927 x1 allowed: nav_summ's default bootstrap seed (not a date); 20260929 x73 allowed: nav_summ's --protocol v8 bootstrap seed and the sprint id platform-v8-20260929 (not a date); 20261003 x6 allowed: R15-SEAL-C).
**Next parent: `scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json`, library v8ysb.**
