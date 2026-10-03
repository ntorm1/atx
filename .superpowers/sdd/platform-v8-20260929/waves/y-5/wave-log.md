### Cell y-5 (rule wave; template `y-two-speed.json` on v8ysb): N 60

**NOT ACCEPTED.** Manifest `scripts/specs/v8/waves/y-5.json` sha256 `f538f9f590570d3c` (commit `43128be8d023`); parent `scripts/specs/v8/lib-v8ysb-gm.json` (library v8ysb, L 1.1828); driver `research_cycle.py wave run`.

Budget v8y-construction-n59-plus-1: construction N 59 -> 60 of 62.

**Cell** `scripts/specs/v8/y-two-speed-y-5-gm.json` (rule, library v8ysb): gross match pm6-6: calibration L 1.1828 G 0.9215182646 vs G_parent 0.9862134133 -> corrected to L 1.2658, G 0.9862947654.

**Mechanics (S2, read before any return): PASS** (gross_all_rows 0.98629 [0.9, 1.05]; abs_net_all_rows 0.0052849 <= 0.02; tau_mean 0.026078 <= 0.2; tau_p95 0.031816 <= 0.3; max_return_identity_error 3.747e-16 <= 1e-09; max_cash_book_relative_error 3.8789e-14 <= 1e-09).

**Statistics of record** (S2): net Sharpe 1.7294 vs parent 1.8495: dSR -0.1201, Memmel SE 0.0753, CBB 95% [-0.26493053102975767, 0.0399586255843882], LW p 0.1398; bundle p one-sided 0.9316, two-sided 0.1398. DSR (N 60): ledger 0.7804; PBO 0.1509.

**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): NOT ACCEPTED** {'criteria': False, 'dsr_positive': False, 'mechanics': True}.
- criterion capacity-4x-higher (printed): unmet
- criterion turnover-per-gross-not-higher (printed): met
- criterion cost-bps-lower (printed): met

Returns (S2, annual): net 5.36% (CAGR 5.45%) vs 5.65%; gross of cost 6.71% vs 7.07%; vol 3.10%; max drawdown 2.67%; 4x net Sharpe 1.6023 vs 1.6994; tau 0.02608 (per unit gross 0.02644).

Ledger `build-equity/trials.jsonl`: lines 130 -> 131, head `6c5f0ff1faca1a1b`, N 59 -> 60, cell trial `cc150c210a3f98e4`; admission lines appended 0.

| phase | run dir | s | peak MiB | outcome |
|---|---|---|---|---|
| u | `build-equity/mega-v8-b0b-train-u-v8ysb-run1` | 12.7 | 530 | completed |
| fit | `build-equity/mega-weights-v8y-two-speed-run1` | 1.0 | 58 | completed |
| w | `build-equity/mega-v8yw-train-two-speed-run1` | 62.2 | 1559 | completed |
| nav | `build-equity/mega-nav-v8y-two-speed-run` | 78.9 | 713 | completed |
| card | `build-equity/mega-cards-v8y-two-speed-run` | 18.8 | 1363 | completed |
| nav | `build-equity/mega-nav-v8y-two-speed-L1.2658-run` | 82.7 | 713 | completed |
| monitor | `build-equity/mega-monitor-v8y-two-speed-run` | 1.6 | 128 | completed |
| summ | `build-equity/cycle-v8y-two-speed-y-5-gm/summ-run1` | 37.7 | 571 | completed |

Hidden-data record: seal scan of 37 log(s) (every run dir, reader and console of the wave; forms iso, compact, year, quarter): 0 date token(s) at or after 2024-01-01 (2026-10-02 x7 allowed: untracked owner plot file name docs/plans/2026-10-02-x5-equity-curve.png in dirty list (PM SEAL-ALLOW); 2026-10-03 x4 allowed: receipt started_utc wall-clock (PM SEAL-ALLOW); 20260927 x1 allowed: nav_summ's default bootstrap seed (not a date); 20260929 x69 allowed: nav_summ's --protocol v8 bootstrap seed and the sprint id platform-v8-20260929 (not a date)).
**Next parent: `scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb.**
