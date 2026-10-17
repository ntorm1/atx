### Cell p9mv-mv-aim (rule wave; template `mv-aim.json` on v8ysb): N 67

**NOT ACCEPTED.** Manifest `scripts/specs/p9/waves/p9mv-mv-aim.json` sha256 `95ab9777cbfe0fb0` (commit `0afd08b4aa83`); parent `scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json` (library v8ysb, L 1.1674); driver `research_cycle.py wave run`.

Budget p9-prereg-construction: construction N 66 -> 67 of 71.

**Cell** `scripts/specs/p9/templates/mv-aim-v1-gm.json` (rule, library v8ysb): gross match pm6-6-engine: calibration L 1.1674 G 0.9624983220 vs G_parent 0.9862418937 -> corrected to L 1.1962, G 0.9863076444.

**Mechanics (S2, read before any return): PASS** (gross_all_rows 0.98631 [0.9, 1.05]; abs_net_all_rows 0.0022709 <= 0.02; tau_mean 0.030306 <= 0.2; tau_p95 0.036467 <= 0.3; max_return_identity_error 3.6191e-16 <= 1e-09; max_cash_book_relative_error 5.8251e-14 <= 1e-09).

**Statistics of record** (S2): net Sharpe 1.4444 vs parent 1.8750: dSR -0.4306, Memmel SE 0.3823, CBB 95% [-1.2233463552066781, 0.34150985940305406], LW p 0.2802; bundle p one-sided 0.8612, two-sided 0.2802. DSR (N 67): ledger 0.5749; PBO 0.1856.

**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): NOT ACCEPTED** {'criteria': False, 'dsr_positive': False, 'mechanics': True}.
- criterion turnover-per-gross-not-higher (printed): unmet
- criterion capacity-4x-higher (printed): unmet
- criterion cost-bps-lower (printed): met

Returns (S2, annual): net 2.32% (CAGR 2.34%) vs 5.69%; gross of cost 3.61% vs 7.06%; vol 1.61%; max drawdown 1.34%; 4x net Sharpe 1.2823 vs 1.7596; tau 0.03031 (per unit gross 0.03073).

Ledger `build-equity/trials.jsonl`: lines 137 -> 138, head `89d3dd8808be3485`, N 66 -> 67, cell trial `ff4ebc47d077f89a`; admission lines appended 0.

| phase | run dir | s | peak MiB | outcome |
|---|---|---|---|---|
| u | `build-equity/mega-p9l-train-u-label-terminal-run1` | 51.6 | 933 | completed |
| fit | `build-equity/mega-weights-p9-theme-cov-hedge-run1` | 3.1 | 66 | completed |
| w | `build-equity/mega-p9w-train-theme-cov-hedge-run1` | 35.6 | 1757 | completed |
| card | `build-equity/mega-cards-p9-theme-cov-hedge-run` | 24.8 | 1441 | completed |
| nav | `build-equity/mega-nav-p9-mv-aim-gm-run` | 26.1 | 1182 | completed |
| monitor | `build-equity/mega-monitor-p9-mv-aim-run` | 1.6 | 131 | completed |
| summ | `build-equity/cycle-mv-aim-v1-gm/summ-run1` | 8.9 | 70 | completed |
| reader:book | `build-equity/waves/p9mv-mv-aim/readers/book-run1` | 0.5 | 45 | completed |
| reader:mech-matched | `build-equity/waves/p9mv-mv-aim/readers/mech-matched-run1` | 0.3 | 5 | completed |
| reader:mech-parent | `build-equity/waves/p9mv-mv-aim/readers/mech-parent-run1` | 0.5 | 44 | completed |
| bundle | `build-equity/waves/p9mv-mv-aim/bundle-run1` | 0.5 | 48 | completed |

Stage seconds: preflight 0.4, register 0.0, screen 0.0, spec 0.6, run 0.6, match 29.9, verify 0.1, judge 14.0.

Hidden-data record: seal scan of 40 log(s) (every run dir, reader and console of the wave; forms iso, compact, year, quarter): 0 date token(s) at or after 2024-01-01 (2026-10-02 x7 allowed: SEAL-ALLOW; 2026-10-03 x21 allowed: R15-SEAL-D; 2026-10-06 x7 allowed: R15-SEAL-D; 2026-10-17 x4 allowed: SEAL-ALLOW; 20260927 x1 allowed: nav_summ's default bootstrap seed (not a date); 20260929 x76 allowed: nav_summ's --protocol v8 bootstrap seed and the sprint id platform-v8-20260929 (not a date); 20261003 x6 allowed: R15-SEAL-C).
**Next parent: `scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json`, library v8ysb.**
