### Cell y-3 (rule wave; template `y-norm-score.json` on v8ysb): N 58

**NOT ACCEPTED.** Manifest `scripts/specs/v8/waves/y-3.json` sha256 `a49e53874a3432ed` (commit `c18a92b7c0ec`); parent `scripts/specs/v8/lib-v8ysb-gm.json` (library v8ysb, L 1.1828); driver `research_cycle.py wave run`.

Budget v8y-construction-n57-plus-1: construction N 57 -> 58 of 62.

**Cell** `scripts/specs/v8/y-norm-score-y-3-gm.json` (rule, library v8ysb): gross match pm6-6: calibration L 1.1828 G 1.0016936049 vs G_parent 0.9862134133 -> corrected to L 1.1645, G 0.9861866382.

**Mechanics (S2, read before any return): PASS** (gross_all_rows 0.98619 [0.9, 1.05]; abs_net_all_rows 0.0057379 <= 0.02; tau_mean 0.029332 <= 0.2; tau_p95 0.034728 <= 0.3; max_return_identity_error 3.592e-16 <= 1e-09; max_cash_book_relative_error 8.4431e-14 <= 1e-09).

**Statistics of record** (S2): net Sharpe 1.8119 vs parent 1.8495: dSR -0.0376, Memmel SE 0.0401, CBB 95% [-0.10597929739157326, 0.028037769129756952], LW p 0.2690; bundle p one-sided 0.8644, two-sided 0.2690. DSR (N 58): ledger 0.8221; PBO 0.0765.

**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): NOT ACCEPTED** {'criteria': False, 'dsr_positive': False, 'mechanics': True}.
- criterion capacity-4x-higher (printed): unmet
- criterion turnover-per-gross-not-higher (printed): unmet
- criterion cost-bps-lower (printed): unmet

Returns (S2, annual): net 5.83% (CAGR 5.95%) vs 5.65%; gross of cost 7.29% vs 7.07%; vol 3.22%; max drawdown 2.70%; 4x net Sharpe 1.6874 vs 1.6994; tau 0.02933 (per unit gross 0.02974).

Ledger `build-equity/trials.jsonl`: lines 128 -> 129, head `a083edaa836b69ef`, N 57 -> 58, cell trial `ee5487109c705108`; admission lines appended 0.

| phase | run dir | s | peak MiB | outcome |
|---|---|---|---|---|
| u | `build-equity/mega-v8-b0b-train-u-v8ysb-run1` | 12.7 | 530 | completed |
| fit | `build-equity/mega-weights-v8x-theme-erc-v8ysb-run1` | 1.9 | 58 | completed |
| w | `build-equity/mega-v8xw-train-theme-erc-v8ysb-run2` | 52.8 | 1436 | completed |
| nav | `build-equity/mega-nav-v8y-norm-score-run` | 54.6 | 586 | completed |
| card | `build-equity/mega-cards-v8x-theme-erc-v8ysb-run` | 24.5 | 1524 | completed |
| monitor | `build-equity/mega-monitor-v8x-theme-erc-v8ysb-run` | 1.6 | 128 | completed |
| nav | `build-equity/mega-nav-v8y-norm-score-L1.1645-run` | 50.9 | 586 | completed |
| summ | `build-equity/cycle-v8y-norm-score-y-3-gm/summ-run1` | 37.2 | 580 | completed |

Hidden-data record: seal scan of 39 log(s) (every run dir, reader and console of the wave; forms iso, compact, year, quarter): 0 date token(s) at or after 2024-01-01 (2026-10-02 x7 allowed: untracked owner plot file name docs/plans/2026-10-02-x5-equity-curve.png in dirty list (PM SEAL-ALLOW); 2026-10-03 x4 allowed: receipt started_utc wall-clock (PM SEAL-ALLOW); 20260927 x1 allowed: nav_summ's default bootstrap seed (not a date); 20260929 x67 allowed: nav_summ's --protocol v8 bootstrap seed and the sprint id platform-v8-20260929 (not a date)).
**Next parent: `scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb.**
