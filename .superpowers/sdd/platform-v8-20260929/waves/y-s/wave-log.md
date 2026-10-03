### Cell y-s (library wave; library v8ys screen, then v8ysb on v8x3b): N 57

**ACCEPTED.** Manifest `scripts/specs/v8/waves/y-s.json` sha256 `22b5534f79dae93f` (commit `4492f3015346`); parent `scripts/specs/v8/x-theme-erc-gm.json` (library v8x3b, L 1.1720); driver `research_cycle.py wave run`.

Budget v8x-hand-25-plus-y-15: admission trials 25 + 15 new = 40 of 40 (cycles v8x*, v8ys*; re-screens left out); construction N 56 -> 57 of 62.

**Screen** (`scripts/specs/v8/lib-v8ys.json`, gate exit 0, sign rule pm7-35): kept so_wang_rev, iv_vol_of_vol, day_rev_freq, mom_turn, ea_uvol, dato, fscore_hbm, stio_trade, deal_target; dropped peer_mom_1m, exch_switch, ins_cluster, smile_slope, div_event, conn_rev.

| id | kind | prior | status | runner sign | decision | reason |
|---|---|---|---|---|---|---|
| peer_mom_1m | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |
| so_wang_rev | add | +1 | reject_turnover | 1 | keep | addition not admitted (status reject_turnover): stays at weight 0 |
| iv_vol_of_vol | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| day_rev_freq | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| mom_turn | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| ea_uvol | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| dato | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| fscore_hbm | add | +1 | admitted | 0 | keep | addition admitted, runner sign 0 (R-2 precedent) |
| exch_switch | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |
| ins_cluster | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |
| smile_slope | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |
| stio_trade | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| div_event | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |
| deal_target | add | +1 | admitted | 1 | keep | addition admitted with the prior sign |
| conn_rev | add | +1 | admitted | -1 | drop | addition admitted with runner sign -1 against prior +1: dropped from the wave |

**Cell** `scripts/specs/v8/lib-v8ysb-gm.json` (b-library, library v8ysb): gross match pm6-6: calibration L 1.1720 G 0.9772114158 vs G_parent 0.9862260459 -> corrected to L 1.1828, G 0.9862134133.

**Mechanics (S2, read before any return): PASS** (gross_all_rows 0.98621 [0.9, 1.05]; abs_net_all_rows 0.00534 <= 0.02; tau_mean 0.028134 <= 0.2; tau_p95 0.03341 <= 0.3; max_return_identity_error 3.6754e-16 <= 1e-09; max_cash_book_relative_error 1.1074e-13 <= 1e-09).

**Statistics of record** (S2): net Sharpe 1.8495 vs parent 1.7695: dSR +0.0800, Memmel SE 0.1502, CBB 95% [-0.23580753873511617, 0.4071793491746803], LW p 0.6262; bundle p one-sided 0.3122, two-sided 0.6262. DSR (N 57): ledger 0.8468; PBO 0.0766.

**Verdict (pm7-34: paired S2 net dSR > 0 AND mechanics (PM7-34); criteria printed, decide nothing): ACCEPTED** {'criteria': False, 'dsr_positive': True, 'mechanics': True}.
- criterion capacity-4x-higher (printed): met
- criterion turnover-per-gross-not-higher (printed): unmet
- criterion cost-bps-lower (printed): unmet

Returns (S2, annual): net 5.65% (CAGR 5.77%) vs 5.08%; gross of cost 7.07% vs 6.45%; vol 3.06%; max drawdown 2.58%; 4x net Sharpe 1.6994 vs 1.6549; tau 0.02813 (per unit gross 0.02853).

Ledger `build-equity/trials.jsonl`: lines 112 -> 128, head `e186895aeed3f7f3`, N 56 -> 57, cell trial `11c10defb3cf38a5`; admission lines appended 15.

| phase | run dir | s | peak MiB | outcome |
|---|---|---|---|---|
| u | `build-equity/mega-v8-b0b-train-u-v8ysb-run1` | 12.7 | 530 | completed |
| fit | `build-equity/mega-weights-v8x-theme-erc-v8ysb-run1` | 1.9 | 58 | completed |
| w | `build-equity/mega-v8xw-train-theme-erc-v8ysb-run2` | 52.8 | 1436 | completed |
| nav | `build-equity/mega-nav-v8x-theme-erc-L1.1720-v8ysb-run` | 59.7 | 586 | completed |
| card | `build-equity/mega-cards-v8x-theme-erc-v8ysb-run` | 24.5 | 1524 | completed |
| ref | `build-equity/mega-nav-v8x-theme-erc-L1.1720-v8ysb-ref-run` | 61.9 | 586 | completed |
| nav | `build-equity/mega-nav-v8x-theme-erc-L1.1828-v8ysb-run` | 55.6 | 586 | completed |
| monitor | `build-equity/mega-monitor-v8x-theme-erc-v8ysb-run` | 1.6 | 128 | completed |
| summ | `build-equity/cycle-v8ysb-gm/summ-run1` | 40.0 | 626 | completed |

Hidden-data record: seal scan of 79 log(s) (every run dir, reader and console of the wave; forms iso, compact, year, quarter): 0 date token(s) at or after 2024-01-01 (2026-10-02 x10 allowed: untracked owner plot file name docs/plans/2026-10-02-x5-equity-curve.png in dirty list (PM SEAL-ALLOW); 2026-10-03 x4 allowed: receipt started_utc wall-clock (PM SEAL-ALLOW); 20260927 x1 allowed: nav_summ's default bootstrap seed (not a date); 20260929 x66 allowed: nav_summ's --protocol v8 bootstrap seed and the sprint id platform-v8-20260929 (not a date)).
**Next parent: `scripts/specs/v8/lib-v8ysb-gm.json`, library v8ysb.**
