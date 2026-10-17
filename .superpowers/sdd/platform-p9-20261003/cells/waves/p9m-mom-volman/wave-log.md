### Cell p9m-mom-volman (rule wave; template `mom-volman.json` on v8ysb): N 65

**NOT ACCEPTED.** Manifest `scripts/specs/p9/waves/p9m-mom-volman.json` sha256 `0bfdd92284afc520` (commit `a11bfb9d2c65`); parent `scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json` (library v8ysb, L 1.1674); driver `research_cycle.py wave run`.

Budget p9-prereg-construction: construction N 64 -> 65 of 71.

**Cell** `scripts/specs/p9/templates/mom-volman-v1-gm.json` (rule, library v8ysb): gross match pm6-6-engine: calibration L 1.1674 G 0.9871897028 vs G_parent 0.9862418937 (stands).

**Mechanics (S2, read before any return): PASS** (gross_all_rows 0.98719 [0.9, 1.05]; abs_net_all_rows 0.005052 <= 0.02; tau_mean 0.026889 <= 0.2; tau_p95 0.032196 <= 0.3; max_return_identity_error 3.8706e-16 <= 1e-09; max_cash_book_relative_error 8.6592e-14 <= 1e-09).

**Statistics of record** (S2): net Sharpe 1.8367 vs parent 1.8750: dSR -0.0383, Memmel SE 0.0525, CBB 95% [-0.10718350908522657, 0.03199132717443943], LW p 0.2860; bundle p one-sided 0.8498, two-sided 0.2860. DSR (N 65): ledger 0.8239; PBO 0.1354.

**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): NOT ACCEPTED** {'criteria': False, 'dsr_positive': False, 'mechanics': True}.
- criterion capacity-4x-higher (printed): unmet
- criterion turnover-per-gross-not-higher (printed): met

Returns (S2, annual): net 5.84% (CAGR 5.96%) vs 5.69%; gross of cost 7.21% vs 7.06%; vol 3.18%; max drawdown 2.26%; 4x net Sharpe 1.7328 vs 1.7596; tau 0.02689 (per unit gross 0.02724).

Ledger `build-equity/trials.jsonl`: lines 135 -> 136, head `daa7783f00183fd1`, N 64 -> 65, cell trial `2355b99d4f19143d`; admission lines appended 0.

| phase | run dir | s | peak MiB | outcome |
|---|---|---|---|---|
| u | `build-equity/mega-p9l-train-u-label-terminal-run1` | 51.6 | 933 | completed |
| fit | `build-equity/mega-weights-p9-mom-volman-run1` | 2.5 | 66 | completed |
| w | `build-equity/mega-p9w-train-mom-volman-run1` | 42.4 | 1757 | completed |
| card | `build-equity/mega-cards-p9-mom-volman-run` | 32.3 | 1379 | completed |
| nav | `build-equity/mega-nav-p9-mom-volman-gm-run` | 45.7 | 693 | completed |
| monitor | `build-equity/mega-monitor-p9-mom-volman-run` | 3.0 | 134 | completed |
| summ | `build-equity/cycle-mom-volman-v1-gm/summ-run1` | 14.0 | 70 | completed |
| reader:book | `build-equity/waves/p9m-mom-volman/readers/book-run1` | 0.8 | 40 | completed |
| reader:mech-matched | `build-equity/waves/p9m-mom-volman/readers/mech-matched-run1` | 0.8 | 39 | completed |
| reader:mech-parent | `build-equity/waves/p9m-mom-volman/readers/mech-parent-run1` | 0.8 | 34 | completed |
| bundle | `build-equity/waves/p9m-mom-volman/bundle-run1` | 1.1 | 54 | completed |

Stage seconds: preflight 0.7, register 0.0, screen 0.0, spec 1.3, run 81.5, match 53.9, verify 0.2, judge 24.0.

Hidden-data record: seal scan of 40 log(s) (every run dir, reader and console of the wave; forms iso, compact, year, quarter): 0 date token(s) at or after 2024-01-01 (2026-10-02 x7 allowed: SEAL-ALLOW; 2026-10-03 x21 allowed: R15-SEAL-D; 2026-10-06 x7 allowed: R15-SEAL-D; 2026-10-17 x4 allowed: SEAL-ALLOW; 20260927 x1 allowed: nav_summ's default bootstrap seed (not a date); 20260929 x74 allowed: nav_summ's --protocol v8 bootstrap seed and the sprint id platform-v8-20260929 (not a date); 20261003 x6 allowed: R15-SEAL-C).
**Next parent: `scripts/specs/p9/templates/theme-cov-hedge-v1-attempt2-gm.json`, library v8ysb.**
