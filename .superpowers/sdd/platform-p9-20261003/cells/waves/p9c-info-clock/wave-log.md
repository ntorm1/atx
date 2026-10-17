### Cell p9c-info-clock (rule wave; template `info-clock.json` on v8ysb): N 66

**NOT ACCEPTED.** Manifest `scripts/specs/p9/waves/p9c-info-clock.json` sha256 `a2975f0c58692f84` (commit `0ca1456480bf`); parent `scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json` (library v8ysb, L 1.1674); driver `research_cycle.py wave run`.

Budget p9-prereg-construction: construction N 65 -> 66 of 71.

**Cell** `scripts/specs/p9/templates/info-clock-rate-v1-gm.json` (rule, library v8ysb): gross match pm6-6-engine: calibration L 1.1674 G 1.0243774148 vs G_parent 0.9862418937 -> corrected to L 1.1239, G 0.9862042241.

**Mechanics (S2, read before any return): PASS** (gross_all_rows 0.9862 [0.9, 1.05]; abs_net_all_rows 0.0065487 <= 0.02; tau_mean 0.033057 <= 0.2; tau_p95 0.049453 <= 0.3; max_return_identity_error 3.6212e-16 <= 1e-09; max_cash_book_relative_error 6.8263e-14 <= 1e-09).

**Statistics of record** (S2): net Sharpe 1.7377 vs parent 1.8750: dSR -0.1373, Memmel SE 0.0835, CBB 95% [-0.3008617113239355, 0.02371149660595964], LW p 0.0994; bundle p one-sided 0.9450, two-sided 0.0994. DSR (N 66): ledger 0.7710; PBO 0.1349.

**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): NOT ACCEPTED** {'criteria': False, 'dsr_positive': False, 'mechanics': True}.
- criterion capacity-4x-higher (printed): unmet
- criterion cost-bps-lower (printed): unmet

Returns (S2, annual): net 5.30% (CAGR 5.39%) vs 5.69%; gross of cost 6.93% vs 7.06%; vol 3.05%; max drawdown 1.99%; 4x net Sharpe 1.6635 vs 1.7596; tau 0.03306 (per unit gross 0.03352).

Ledger `build-equity/trials.jsonl`: lines 136 -> 137, head `acab9e0eea9b1432`, N 65 -> 66, cell trial `52988f05af490865`; admission lines appended 0.

| phase | run dir | s | peak MiB | outcome |
|---|---|---|---|---|
| u | `build-equity/mega-p9l-train-u-label-terminal-run1` | 51.6 | 933 | completed |
| fit | `build-equity/mega-weights-p9-theme-cov-hedge-run1` | 3.1 | 66 | completed |
| w | `build-equity/mega-p9w-train-theme-cov-hedge-run1` | 35.6 | 1757 | completed |
| card | `build-equity/mega-cards-p9-theme-cov-hedge-run` | 24.8 | 1441 | completed |
| nav | `build-equity/mega-nav-p9-info-clock-gm-run` | 48.5 | 693 | completed |
| monitor | `build-equity/mega-monitor-p9-info-clock-run` | 2.4 | 133 | completed |
| summ | `build-equity/cycle-info-clock-rate-v1-gm/summ-run1` | 16.2 | 70 | completed |
| reader:book | `build-equity/waves/p9c-info-clock/readers/book-run1` | 0.5 | 35 | completed |
| reader:mech-matched | `build-equity/waves/p9c-info-clock/readers/mech-matched-run1` | 0.8 | 42 | completed |
| reader:mech-parent | `build-equity/waves/p9c-info-clock/readers/mech-parent-run1` | 0.5 | 35 | completed |
| bundle | `build-equity/waves/p9c-info-clock/bundle-run1` | 0.5 | 39 | completed |

Stage seconds: preflight 0.6, register 0.0, screen 0.0, spec 1.0, run 1.2, match 54.9, verify 0.2, judge 23.9.

Hidden-data record: seal scan of 40 log(s) (every run dir, reader and console of the wave; forms iso, compact, year, quarter): 0 date token(s) at or after 2024-01-01 (2026-10-02 x7 allowed: SEAL-ALLOW; 2026-10-03 x21 allowed: R15-SEAL-D; 2026-10-06 x7 allowed: R15-SEAL-D; 2026-10-17 x4 allowed: SEAL-ALLOW; 20260927 x1 allowed: nav_summ's default bootstrap seed (not a date); 20260929 x75 allowed: nav_summ's --protocol v8 bootstrap seed and the sprint id platform-v8-20260929 (not a date); 20261003 x6 allowed: R15-SEAL-C).
**Next parent: `scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json`, library v8ysb.**
