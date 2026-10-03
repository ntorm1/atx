### Cell y-2 (rule wave; template `y-theme-tsmom.json` on v8ysb): N 59

**NOT ACCEPTED.** Manifest `scripts/specs/v8/waves/y-2.json` sha256 `3655181d316f6548` (commit `15017bb41023`); parent `scripts/specs/v8/lib-v8ysb-gm.json` (library v8ysb, L 1.1828); driver `research_cycle.py wave run`.

Budget v8y-construction-n58-plus-1: construction N 58 -> 59 of 62.

**Cell** `scripts/specs/v8/y-theme-tsmom-y-2-gm.json` (rule, library v8ysb): gross match pm6-6: calibration L 1.1828 G 0.9689836621 vs G_parent 0.9862134133 -> corrected to L 1.2038, G 0.9862340550.

**Mechanics (S2, read before any return): PASS** (gross_all_rows 0.98623 [0.9, 1.05]; abs_net_all_rows 0.0048411 <= 0.02; tau_mean 0.030219 <= 0.2; tau_p95 0.04759 <= 0.3; max_return_identity_error 4.3639e-16 <= 1e-09; max_cash_book_relative_error 3.8829e-14 <= 1e-09).

**Statistics of record** (S2): net Sharpe 1.5182 vs parent 1.8495: dSR -0.3313, Memmel SE 0.2307, CBB 95% [-1.0569527140577306, 0.23048206379363126], LW p 0.3976; bundle p one-sided 0.8794, two-sided 0.3976. DSR (N 59): ledger 0.6457; PBO 0.1511.

**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): NOT ACCEPTED** {'criteria': False, 'dsr_positive': False, 'mechanics': True}.
- criterion capacity-4x-higher (printed): unmet
- criterion turnover-per-gross-not-higher (printed): unmet
- criterion cost-bps-lower (printed): met

Returns (S2, annual): net 4.79% (CAGR 4.85%) vs 5.65%; gross of cost 6.25% vs 7.07%; vol 3.15%; max drawdown 3.39%; 4x net Sharpe 1.3498 vs 1.6994; tau 0.03022 (per unit gross 0.03064).

Ledger `build-equity/trials.jsonl`: lines 129 -> 130, head `58bce60efae09d44`, N 58 -> 59, cell trial `aeeb2073e0bd8e2a`; admission lines appended 0.

| phase | run dir | s | peak MiB | outcome |
|---|---|---|---|---|
| u | `build-equity/mega-v8-b0b-train-u-v8ysb-run1` | 12.7 | 530 | completed |
| fit | `build-equity/mega-weights-v8y-theme-tsmom-run1` | 0.8 | 57 | completed |
| w | `build-equity/mega-v8yw-train-theme-tsmom-run1` | 42.2 | 1432 | completed |
| nav | `build-equity/mega-nav-v8y-theme-tsmom-run` | 43.5 | 586 | completed |
| card | `build-equity/mega-cards-v8y-theme-tsmom-run` | 18.2 | 1396 | completed |
| nav | `build-equity/mega-nav-v8y-theme-tsmom-L1.2038-run` | 47.4 | 586 | completed |
| monitor | `build-equity/mega-monitor-v8y-theme-tsmom-run` | 1.6 | 130 | completed |
| summ | `build-equity/cycle-v8y-theme-tsmom-y-2-gm/summ-run1` | 34.4 | 609 | completed |

Hidden-data record: seal scan of 37 log(s) (every run dir, reader and console of the wave; forms iso, compact, year, quarter): 0 date token(s) at or after 2024-01-01 (2026-10-02 x7 allowed: untracked owner plot file name docs/plans/2026-10-02-x5-equity-curve.png in dirty list (PM SEAL-ALLOW); 2026-10-03 x4 allowed: receipt started_utc wall-clock (PM SEAL-ALLOW); 20260927 x1 allowed: nav_summ's default bootstrap seed (not a date); 20260929 x68 allowed: nav_summ's --protocol v8 bootstrap seed and the sprint id platform-v8-20260929 (not a date)).
**Next parent: `scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb.**
