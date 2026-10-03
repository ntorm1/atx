# Platform v8 — status 7 (PM session 8, 2026-10-02 evening)

Breakpoint: every expansion-Y lane delivered, reviewed and merged; build v8-16d green; X-5 identity run and the Y cells
are next in root. This file is the owner's status at that breakpoint. Numbers below are the public ledger numbers;
nothing dated 2024-01-01 or later has been opened; the hidden block and the OD-3 history read remain closed.

## 1. The book today

| book | S2 net Sharpe | net annual | gross of cost | 4x net Sharpe | turnover / day | max DD | N |
|---|---|---|---|---|---|---|---|
| X-5 `theme-erc-v1` (accepted, deployable under PM7-34) | **1.7695** | **5.08%** | 6.45% | 1.655 | .0268 | 2.06% | 54 |
| V8-F (pre-X baseline) | ~1.42 | — | — | — | — | — | 50 |

- X-5 vs R-2 cumulative paired dSR +.5135 (SE .184, p one-sided .003). X-5's own gain (+.349) decomposes as lower
  volatility +.311, gross alpha +.080, cost −.042; the adversarial review puts the out-of-sample expectation near +.15.
- Deflated Sharpe at N 56 (after X-7): .731. N_tot 212 (102 hand-written + 110 campaign evaluations).
- Leverage ~1.17. The book is return-starved, not risk-starved: 2.9% volatility, 5% net. The remaining levers are the
  Y rules (concentration, theme timing, two-speed), the 15 Y signals, and then leverage (X-10 at L 2.0; Y-1 vol-managed).

## 2. What happened this session (X batch 2-3)

| step | result |
|---|---|
| X-5 identity under v8-15 (price_volume theme, C++ list) | NAV 27/27 identical; w 10/12 (2 differ in timing only); fit identical after PM7-30 substitution; ic-tests 159/159 |
| Fields v14 (open/high/low_adj) | manifest `4b12c0e1…cbb0c`, 75 reused / 3 computed |
| **X-7 formulaic-alpha wave** (12 Kakushadze strings) | gate 8/12 admitted with the prior sign; 2 dropped on opposite sign (PM7-35); cell ran on 10 additions; **NOT ACCEPTED**: dSR −.161 (SE .193, p .81), net 4.61%, turnover +33% (.0356), N 56 |
| **Campaign v9-mine-c1** (110 evaluations, 10 fields, 4 workers) | mechanics pass; best t 5.00 vs hurdle 5.40; **0 shortlisted, 0 admitted**; X-9 undefined |
| Side fixes | miner relative-path launch on Windows (`research_mine.py`, with test); NULL_PINS in gm specs |

Reading: hand-transcribed published formulas and blind mining both failed to add to a 58-member book this round. The
formulaic class is parked (PM8-17).

## 3. Expansion Y — delivered (all blind; no Y statistic exists)

| lane | pool / head | delivered | review |
|---|---|---|---|
| YSIG (signals, round 2) | 12 / `5431831a` | 10 candidates with horizon class + half-life: peer_mom_1m, so_wang_rev, iv_vol_of_vol, day_rev_freq, mom_turn, ea_uvol, dato, fscore_hbm, exch_switch, ins_cluster; `ysig_check.py` (25 planted errors fail) | checker |
| YDATA (data) | 13 / `e1cf4135` | no in-house VWAP (owner decision: buy, or keep 43 V-formulas out); fields iv_skew_21, stio_chg_q, div_init_omit, deal_pending, conn_ret63 (13F connected stocks); 5 candidates: smile_slope, stio_trade, div_event, deal_target (new theme merger_arbitrage), conn_rev; data asks: securities lending, SG&A/R&D history, N-PORT, Lazy Prices | tools suite 340 |
| YCOMB (rules) | 14 / `cc08ac35` | Y-1 vol-target-v1 (Moreira-Muir, L_t = clip(2·σ_ref/σ̂, 1, 2)); Y-2 theme-tsmom-v1 (walk-forward factor momentum on theme sleeves); Y-3 norm-score-v1 (rank → normal score); Y-5 two-speed-v1 (fast/slow sleeves netted before trading; owner's multi-horizon ask, PM8-5) — all C++, Python writes specs only | adversarial: 2 MAJOR + 14 MINOR, all fixed |
| YINFRA (RSI tooling) | 15 / `85a98a5b` | one-command wave driver (9 resumable stages with receipts, seal scan, budget check), candidate queue, scoreboard, wave speedups (marginal pass no longer run twice ≈ 58% of wave phase time) | adversarial: 7 MAJOR + 11 MINOR, all fixed; suites 304/0, 308/0 |
| YOPS (DSL ops) | 12 / `b25c2a2f` | group_sum, group_delay, as-of-rank family (opcodes 105-111); 10 of 12 non-VWAP formulaic alphas now exact; registrations untouched; parked at 0 trials (PM8-17) | built: 16/16 after fixture fix |
| YPRE (pre-registration) | 13 / `87a9e0f4` | `v8y-prereg.md`: 15 hand-written Y trials, 6 Y cells, deflation counts, fields v15, 36 SHA-256 recomputed (0 mismatch); 15 open choices ruled PM8-14 | — |
| YARCH (platform) | 16 / `88cd7d49`+ | audit + migration plan (PM8-12): C++ field-builder library (29→31/31 tests), spec-driven generator replacing generate_fund_ic_v4/v6/v70/v71 byte-for-byte; engine-fields exe; slices 3b-4 in progress | — |

Integration (root, pool-2 `feat/platform-v8-20260929`): all seven merged; theme `merger_arbitrage` 13th (Python + C++
list); iv_vol_of_vol IV-clock repair (0 trials); Y field registry rows; one first-compile fix (ADL ambiguity);
**v8-16d builds clean, 0 warnings**: ic-tests 170/170, target-tests 321/321, book 181/181, VM-sources tripwire passes.
Release-IC switch deferred (its tree was removed by cleanup; Debug stays).

## 4. Rulings this session (PM8-1 … PM8-18; verbatim in `progress.md` "PM session 8")

- PM8-1 acceptance stays PM7-34 (dSR > 0 AND mechanics; capacity printed only). PM8-2 expansion Y opened.
  PM8-3 X-10 leverage moved after the Y cells. PM8-4 root runs cells without returning.
- PM8-5 multi-horizon rule (owner note) → Y-5. PM8-6/8/11 YSIG, YDATA rulings; roster cap 80 → 96.
- PM8-9 merge order; first real wave stage by stage. PM8-10 Y cell order: **Y-S → Y-3 → Y-2 → Y-5 → X-10 → Y-1 →
  adoption print → OD-3**. PM8-12 **architecture directive**: core logic in C++ engine, Python thin wrapper, no
  versioned script copies. PM8-13 half-life table pinned. PM8-14 v8y-prereg ruled. PM8-15 marginal cap 720 s.
  PM8-16 YCOMB registration fixes. PM8-17 formulaic strings parked. PM8-18 YARCH slices.

## 5. Next (root, in order)

1. X-5 identity under v8-16d, cold candidate cache (every new flag absent).
2. Fields v15 (v14 + exch_up_365d, iv_skew_21, stio_chg_q, div_init_omit, deal_pending, conn_ret63); IC memory re-probe
   (Y-S screen library ≈ 3,200 MiB estimated vs 2,560 cap).
3. **Y-S**: 15 strings through the admission gate (sign rule PM7-35), one add-alpha wave on X-5; via the driver stage
   by stage, hand count checked at preflight (X hand-written 25 + Y 15).
4. Y-3 norm-score → Y-2 theme-tsmom → Y-5 two-speed (each: dSR > 0 AND mechanics; turnover, cost/traded $, 4x printed).
5. X-10 (L 2.0) → Y-1 vol-target (by hand; section 8 criteria) → adoption print (S2 net Sharpe ≥ 1.0 baseline, DSR,
   p, 4x, year table) → stop before OD-3.
6. Final report: unlevered Y-F0 and levered books side by side; the owner's leverage decision.

## 6. Owner decisions outstanding

1. **VWAP data purchase** (unlocks 43 formulaic alphas; doc `docs/plans/2026-10-02-v8y-vwap-data-ask.md`) — note the
   formulaic class just lost at X-7; low priority unless cheap.
2. **Securities-lending data** (utilisation, borrow fee, lendable supply): short_interest theme has no new candidates
   without it.
3. **Leverage / production**: after the adoption print, which book to deploy (unlevered Y-F0 vs X-10 vs Y-1). The
   "minimum baseline" is registered as S2 net Sharpe ≥ 1.0; the return hurdle for production is the owner's number —
   please state it so the final report can print pass / fail against it.
4. Platform migration pace (PM8-12): YARCH's plan retires ~19k Python lines in 5 slices; slices 1-3 are in; whether
   slices 4-5 (composition mirrors, admission / ledger statistics) run before or after v9.

## 7. Housekeeping

- Disk: pool-2 87 → 43 GB; C: 84 GB free (23 GiB freed; every receipt / summary / manifest kept; `cleanup-step-c.md`).
  Remaining deletable after the final report: lo1 v9 + cache (6.9 GiB) once the pitch config points at lo3.
- Worktrees: pools 12-16 hold the Y lane branches (all merged); release after the final report. Pools 7, 8, 10 untouched.
- Known gaps: Y-S IC memory may exceed the cap (re-probe first); ConstructionDay grew ~24 B (parent `--max-bytes`
  headroom checked at identity); #68 / #92 formulaic strings still inexpressible; Y-2 runs 2021-2023 only (large SE).
