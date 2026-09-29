# platform-20260928 -- ledger (newest first; `git add -f` this dir)

Goal (owner, 2026-09-28): continue building atx-engine + atx-impl into a production-quality equity L/S quant platform
(high Sharpe, high capacity, low turnover, low/medium frequency). Three fronts: (1) platform + pipeline for faster research
iteration, (2) the implementation and the mega strategy, (3) the pitch to production. Start with code review + web research
-> plan -> 3-4 Opus 5.5 implementer lanes. Root (pool-2) = PM: builds, real-data runs, acceptance, ledger.

Standing rules carried from mega-alpha-20260926 (unchanged): root alone builds (mega-build.ps1) and runs real data
(run_bounded_research.py 180 s / 1536 MiB); children own pool worktrees, never build, never run real data, never spawn
subagents, trailer Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>, task-TN-report.md, reply < 15 lines; TRAIN
2020-2022 only; 2023-2024 used twice (new read = validation trial #3, owner gate U1); 2025+ reserved; never read a
per-candidate VAL statistic; no pushes, warehouse writes, broker actions; tree clean before every real-data run; every
ruling declared before any measurement it could bias; Appendix A trial accounting on every TRAIN result.

Inputs: mega-alpha-20260926/{v6-code-review-exec.md, v6-code-review-signal.md, v6-literature.md, v4-prereg.md, progress.md},
docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md, docs/plans/2026-09-28-mega-alpha-data-request-atx-db.md,
docs/plans/2026-09-28-mega-alpha-v6-pitch.html (the v6.1 pitch), final cell mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247
(S2 net +1.239, DSR N29 .911 < .95: freeze gate unmet).

## 2026-09-28 W1 merged (f6faa6cd); spo-v1 pre-registered; risk exposures built
- W1 spo-v1 (FISTA/ADMM around the GP aim with factor risk + cost v2; new strategy_spo.{cpp,hpp}). Build mega-v7-w1 exit
  0 (6 TUs, 35 s). Tests 77/78: SpoSolver.NetGrossBetaAndBoxesHoldTo1e10 fails on `multipliers.rho == 0` (gross
  multiplier) -> sent to W1; the cell waits for the fix because gamma is calibrated from the binding gross constraint.
- v7-prereg.md: spo-v1 single cell declared (defaults ic-book .02, w-max .01, adv q .05 / p .01, iters 500, tol 1e-8,
  target-vol .05, gamma = max(vol-5% gamma, gross-1.247 gamma), 20-session cost amortisation, primary books; N 32 -> 33).
- Risk model with per-date exposures (`--emit-exposures all`): build-equity/v7-w1-risk-all, 27.9 s, 495 MiB, 377 MB,
  manifest sha 897ffdf2...b46bd9d19967e (pinned for the cell).

## 2026-09-28 W4 merged (8921dc2c) and ACCEPTED on real data
- Build mega-v7-w4 exit 0 (12 TUs, 54 s), 80/80 gtests (StrategyLive 18 + identity suites). NAV v6.1 flag off 32.8 s /
  flag on with binary holdings 39.8 s (1.21x, cap 1.3x; L3-F1 CLOSED: was 140 s / 374 MB CSV, now 118 MB f64 + index);
  10/10 NAV files identical both ways. decide at the three dates from the binary holdings: exit 0, parity 0, 1.8-2.1 s
  (was 16-17 s), targets.csv/orders.csv byte-identical to the L3 runs, orders_shares.csv written ($1bn NAV, lot 1, min
  notional 500). reconcile self: 1,838 names, 0 breaks; mutated broker file: missing 1, quantity 1, extra 1 unexplained
  (exit 5) and the x2 split explained by corporate actions -> as specified.
- L3-F2 CLOSED as definitional: under the replay's TC definition (desired/sigma) the three dates read .723 / .695 / .755
  (> .5); the .09-.29 values were the signal/sigma^2 definition (R1 M-3), now both reported under explicit keys.
- R1 rows M-1, M-2, M-3, m-8, m-13, m-14 fixed in W4. Remaining review rows: I-1, M-5, M-6, M-7 in F1 (running); M-4
  disclosed (prereg addendum A1).

## 2026-09-28 atx-db delivered most of the data request -> wave 3 opened
- ALPHA_PANEL_REQUEST_V7_RESPONSE.md (atx-db, 20:44): U1 link table met (95.6-99.1% linked/yr), U2 dead-line 91-93%, U3
  security master, U4 delisting 85.8% classified (imputed returns), D1 13F 2013q2-2026q2 (124.4M rows), D2 earnings
  calendar with expected next date, D3 short volume splits + FTD + Reg SHO (NYSE landing still running), D6 all 15
  statement items, D7 history (fundamentals 2010+, prices 2012-03+), D10 corporate actions, D11 Form 4 (4.90M), D12 8-K
  metadata. Not available: consensus, borrow fee/utilisation, GICS/NAICS, options skew/OI, VWAP, index add/drop. S2
  coverage improved but most targets short (gp_ttm .753 linked-USD vs .90; xrd .895 vs .95; fscore_partial .922 MET).
  Panel v2 assembly, borrow proxy, metrics and the lo1 aligned export are pending on their side (1.5-2.5 h).
- Ruling: consume the delivered stage exports directly (not the pending panel) through new PIT field modules; role
  variant linked-operating-v2 on identity-bridge-v2-pit; library v7 pre-registered from a read-only draft before any IC
  run -- data before definitions would bias admission -- cost if wrong: rework when the panel lands. Lanes: W5a pool-8
  (earnings calendar, Form 4, 8-K fields), W5b pool-9 (13F, FTD/Reg SHO fields + role v2 + delisting returns), P3
  read-only library-v7 draft. Owner pointer: C:/atx/atx-db/docs/ALPHA_PANEL_STATUS.md.

## 2026-09-28 R1 review landed (1 I, 7 M, 16 m) -- routed
- I-1 v6 capacity pass prices c_i at base scale (v5 capacity curve unaffected) -> F1. M-1..M-3 decide (flat-book default
  when the as-of row is missing; no positions/locates SHA in decision.json; TC definition differs from the replay's
  v7_transfer_coefficient.csv: signal/sigma^2 vs desired/sigma -> the L3-F2 TC values are not comparable with L4's) -> W4.
  M-4 aim-partial-v6 side rescale undisclosed -> v7-prereg.md Addendum A1 (disclosed deviation; results already read; no
  acceptance depended on it). M-5 bias harness silently drops unforecast factors and uncovered names (b biased up ->
  the .995 reading is optimistic until fixed) -> F1. M-6 --reuse keyed on a hand-bumped revision -> F1. M-7 pitch.py
  cache-scan picks a v2 entry without DSL sha -> F1. F1 lane opened in pool-7.

## 2026-09-28 GRID RESULT: aim-partial-v6 C1-C3 REJECTED; baseline v6.1 stands (N 32)
| cell | kappa | clip | S2 net | gross SR | cost bps/$ | tau mean | dSR vs v6.1 (Memmel SE, t) | DSR N32 |
|---|---|---|---|---|---|---|---|---|
| baseline v6.1 (aim-partial-v5) | - | - | +1.239 | 1.594 | 13.55 | .0379 | - | .9032 |
| C1 | .5 | [.5,1.5] | +1.160 | 1.431 | 11.82 | .0372 | -0.080 (.078, -1.02) | .8790 |
| C2 | 1.0 | [.5,1.5] | +1.121 | 1.424 | 11.34 | .0369 | -0.118 (.103, -1.15) | .8663 |
| C3 | 1.0 | [1,1] | +1.112 | 1.425 | 11.42 | .0367 | -0.127 (.100, -1.27) | .8639 |
- Mechanics pass on all three (gross_lev_all_rows .969-.971, |net| <= .0077, tau p95 <= .047), cost per traded dollar
  falls 13-16%, but gross SR falls ~10%: the cost-scaled target shrink removes more alpha than cost (cheap names are not
  the alpha-rich names). Acceptance rule (dSR > 0) fails for every cell -> REJECTED, no freeze change. Regime rate
  (C1/C2 vs C3) adds nothing measurable. Finding G1 for the SPO lane: shrink targets by alpha-per-cost, not by cost alone
  (that is what the optimiser objective does).
- CSCV PBO over {baseline, C1, C2, C3}: .346 (IS winner = baseline in 9,790 of 12,870 splits; OOS +1.226 ann; P[OOS<0]
  .026). Effective-N (ONC) over the 32 trial series: N_eff 5; ledger 32 lines.
- Appendix A trial accounting: TRAIN construction cells 29 -> 32 (C1-C3 disclosed above, all rejected). Validation
  trials 2 spent (unchanged). 2025+ reserved. No per-candidate VAL statistic read.
- Baseline v6.1 at N 32: effective-N DSR .9512 (N_eff 5) beside cell-count DSR .9032 and Lo .5153. Reported only; the
  freeze gate remains the pre-registered cell-count DSR >= .95 (unmet at .903).
- Note: PBO .35 > .2 threshold in v7-prereg applies only to accepting a NEW winner; the baseline was not chosen on this
  grid, so nothing is accepted or gated by it. Recorded for the pitch's honesty section.

## 2026-09-28 v7-2 build green; identity PASS; grid C1-C3 running; wave 2 dispatched
- Fix-ups merged: L1 short partial-file names (Windows 259-char path limit was the "partial output" cause), L4 risk-verb
  role masks + `--band-exponent` (+ test rename). Build mega-v7-2a/2b: exit 0; StrategyIcRunner 46/46; target tests
  124/124 (L4 + identity + StrategyLive). HEAD debbb27f.
- aim-partial-v6 identity (kappa 0, band exponent 0, clip [1,1]): 10/10 CSVs byte-identical to the v5 cell -> prereg
  identity requirement met; C1-C3 launched (three cells, N 29 -> 32 as pre-registered).
- Risk verb on lo1: exit 0, 31.4 s, 521 MiB; geometry 1,155 dates x 5,627 instruments, 62 factors (market + industries +
  11 styles; descriptors book_to_price, earnings_yield, gross_profitability, asset_growth, leverage, si_ratio, days_to_cover;
  unavailable []). Bias harness factor family full-sample b mean .995 / median .997; rolling-252 b mean .987 within band
  [.911, 1.089]; pooled kurtosis 7.0. Descriptive; ACCEPTED as a working risk model v1.
- Owner (mid-turn): spin up more sub agents, speed up. Ruling: wave 2 dispatched now, before the grid result -- wave-2
  lanes do not touch the pre-registered grid parameters, so nothing they do can bias it -- cost if wrong: rebase work.
  W1 pool-3 spo-v1 optimiser (FISTA/ADMM, factor risk + cost v2), W2 pool-10 DSL ops, W3 pool-11 report card + monitor +
  fitter WorkStore, W4 pool-4 holdings writer + order file + reconcile + freshness/TC checks, R1 adversarial review of
  cdc9c2a8..HEAD (read-only).

## 2026-09-28 L4 real data: stress + capacity ACCEPTED (descriptive); aim-partial-v6 identity FAILED (fix requested)
- `--cost-v2 --capacity-curve` on the v6.1 cell (v7-l4-nav-stress): exit 0, 80.4 s, 368 MiB; S1/S2/S3/flat-300/engine-tiers
  10/10 files byte-identical to the v6.1 cell. New descriptive scenarios (TRAIN, no trial): S2-KO net SR 1.269, S2-FIM 1.347
  vs S2 1.238, S1 1.380, S3 .359 -> S2 remains the conservative primary (P2 S4 expectation confirmed).
- Capacity curve by replay (R4.2; x1 == S2 bit-for-bit): NAV multiple .5/1/2/4/8 -> net SR 1.273/1.239/1.183/1.053/.912;
  gross SR 1.590/1.594/1.578/1.467/1.312; cost 11.5/13.5/16.0/18.2/19.9 bps per traded $; capped-fill share .12%/.51%/
  3.3%/12.1%/26.6%. Read: net SR >= 1.0 holds to ~4x ($4bn at the $1bn base) with the 1% ADV cap binding from 2x.
- aim-partial-v6 identity cell (kappa 0, clip [1,1]): S2 net 1.241 vs 1.238, all 10 files differ from row 0 (banded_names)
  because the band is always cost-scaled with exponent 1/3 and no uniform option exists. Per v7-prereg.md no C1-C3 cell
  runs until identity holds -> L4 asked to add --band-exponent (default 1/3; 0 = uniform). Also pending: the risk verb
  mask fix.

## 2026-09-28 L1 merged (b5554999) -- real-data ACCEPTED, one gtest fix-up pending; L4 merged (6f7ee661) -- build green
- Build mega-v7-l1 (core tests, IC tests, IC exe, target tests): exit 0, 95 TUs, 215.6 s. Sha256.* 7/7; StrategyIcRunner.*
  44/45 (CandidateCacheReadsV1EntriesInPlaceThroughKnownManifests: "candidate cache partial output" -> sent to L1);
  StrategyLive.* 9/9 after the L3 test fix.
- L1 real data (hard-linked v6u cache -> mega-candidate-cache-v7l1, --cache-legacy-fields lo1-fields-v6b): u pass 23.9 s /
  856 MiB (was 76.1-104.9 s / 1,059 MiB), signal + IC cache 38 hit / 1 miss (sv_flow only), orientations.json,
  train_daily_ic.csv, train_combined.json byte-identical to mega-v61-train-u-1, train_candidates identical after the four
  timing/cache drops. w pass 19.3 s / 505 MiB (was 31.5 s), 39/39 hits, identical to mega-v61w-train-ew-1. --cache-report
  works with --max-memory-mib 1536 (without it the admission check refuses; L1-F1 minor). Targets were u <= 20 / w <= 15 s;
  measured under a concurrent 4-job build; accepted on identity + hit counts, timing re-measured on a quiet host later.
- L4 rebased onto L3 (fb5920f0; conflicts in the two CMakeLists and strategy_nav_replay.cpp, hook 14+/4- lines at 10
  seams inside L3's plan_weights). Build mega-v7-l4 (target tests, NAV exe, new atx-equity-strategy-risk): exit 0, 46.6 s,
  9 TUs. 119/119 gtests (L4 suites + identity suites + StrategyLive). Risk verb on the real lo1 role refused:
  "InvalidArgument: risk: role presence/membership masks" -> sent to L4 (must load the restriction masks as the replay does).
  Stress/capacity run and the aim-partial-v6 identity cell in progress.

## 2026-09-28 L2 ACCEPTED; L3 merged (31f79c0e + fix 8f66bd25) and ACCEPTED; L1 follow-up in flight
- L2 items (2)-(4): `run specs/v61.json --suffix r7` reproduced the v6.1 ladder with zero hand pins (u 104.9 s / 1059 MiB,
  fit 34.7 s, w 43.3 s, nav 40.5 s); all 10 daily/events CSVs byte-identical to the v6.1 cell (primary sha 1adf0e8f...).
  `--reuse-fields` from lo1-fields-v6b: 40 reused, computed [sv_ratio126], files map == fields-v7 (41/41). Refusal:
  max_rss_mib=64 -> "HARD-STOP [u]: ... outcome rss-limit, exit_code 15", driver exit 4. L2 ACCEPTED.
- L3: build mega-v7-l3 (exit 0, 52.5 s, 6 TUs, no warnings-as-errors fixes needed). gtests 89/90; the one failure was a test
  bug (two refusal outputs collided with the fixture's nav dir) fixed test-only in c1fa1e75 (merged 8f66bd25; re-run of the
  9 StrategyLive tests pending the next test build). Real data: flag off -> v6.1 cell 11/11 files identical (33.6 s);
  `--emit-holdings` -> 11/11 identical but 140.3 s wall and a 374 MB holdings.csv (finding L3-F1: needs a streaming/binary
  writer or per-year shards before the 180 s cap bites; holdings.csv deleted after acceptance, holdings_days.csv kept).
  decide at 2020-08-06 / 2021-08-04 / 2022-10-12 from the emitted holdings with --check-replay: exit 0, replay_parity
  mismatches 0 at all three, 16-17 s each, health warn. L3 ACCEPTED on the pre-registered criteria.
- Finding L3-F2 (for wave 2): transfer coefficient corr(alpha/sigma^2, w) = .089 / .185 / .262 (target) at the three dates;
  P2 R2.5 flags TC < .5 as constraints (locate, ADV cap, neutrality, partial aim) eating alpha -> supports the SPO lane.
- L1 (pool-10 HEAD 35861db6): SHA-NI + content-keyed cache v2 + --cache-report; flagged that fit_composition_weights.py
  and mega_report/pitch.py read only v1 sidecars -> sent back to L1 to add v2 readers with tests before root builds.

## 2026-09-28 L2 merged (21663eed) -- acceptance in progress
- L2 (pool-11, HEAD 6332d686, 104 tests): research_cycle.py (plan/lock/run/status, hard-stop on refused receipt),
  prepare_research_fields.py --reuse, studies/backtest_integrity.py + nav_summ.py (trial ledger, ONC effective-N DSR,
  CSCV PBO, PSR/MinTRL). Stretch WorkStore skipped (needs L1 sidecar). Merged before L1 (no C++ touched).
- Accepted so far: (1) `plan specs/v61.json --lines-only` == v61_train.sh DRY fixture (diff empty). (5) integrity stats on
  the 29 cells, two runs byte-identical, ledger 29 lines then +0. Final cell v61u L1.247: effective-N DSR .9491 (ONC
  N_eff 5 of 29; mean off-diagonal rho .931) beside cell-count DSR .9108 and Lo .5305; PSR(0) .978, MinTRL 95% 501 sessions
  (1.99 y); PSR(.5) .886. CSCV PBO over all 29 cells .357 (exhaustive 12,870 splits, 16 x 47 sessions; IS winner +1.350 ->
  OOS +.978, P[OOS<0] .069). Reported only; the v6 gate is NOT re-scored (R5.2). Note: the 29-cell grid mixes v5 and v6
  lineages, so this PBO is an upper-bound-ish reading for the v6 ladder; the v7 grid PBO will be over {baseline, C1-C3}.
- Pending: (2) full reproduction run --suffix r7 (S2 daily CSV SHA must equal 1adf0e8f...), (3) --reuse-fields from v6b,
  (4) refusal stop with max_rss_mib=64.

## 2026-09-28 P2 landed; plan v7; lanes L1-L4 dispatched; A5 A/B
- P2 literature-v7.md (451 lines). Top: integrity tooling (trial ledger, effective-N DSR, CSCV PBO) first; cost model v2
  (KO/FIM) + replayed capacity curve; USE4-style risk model + bias harness; GP cost-aware optimiser (L, after risk + cost);
  keep ew-theme-v1 (fitted weights lose 35-60% SR at T = 3 y). Do not build: fitted sleeve weights, stop-losses, finer grids.
- plan-v7.md = synthesis (decisions D1-D10, lanes, contracts, trials). Briefs L1-L4 committed cdc9c2a8; implementers spawned
  (Opus 5.5): L1 pool-10 IC cache/hash, L2 pool-11 research_cycle + --reuse + nav_summ integrity, L3 pool-4 decide path,
  L4 pool-3 risk model + cost v2 + aim-partial-v6. Merge order L1 -> L2 -> L3 -> L4.
- A5 result: build mega-v7-rel0 (equity-rel, exit 0, source 8cb735ab). v6.1 u pass with the Release IC exe on a fresh
  cache (mega-v7rel-train-u-1, receipt source cdc9c2a8): orientations.json, train_daily_ic.csv, train_combined.json
  byte-identical to mega-v61-train-u-1; train_candidates.jsonl identical after dropping timing/cache keys. IDENTITY PASS.
  Wall 98.1 s vs 76.1 s Debug, peak RSS 1,058 vs 1,061 MiB -- measured while four agent lanes hammer the disk; timing
  inconclusive, re-measure on a quiet host before adopting Release. Not adopted yet.
- Ruling: v7-prereg.md declares aim-partial-v6 parameters and the 3-cell grid, S2-KO/S2-FIM as descriptive scenarios, and
  effective-N DSR reporting from v7 on -- declared before any L4 measurement -- cost if wrong: 3 cells of N.

## 2026-09-28 sprint open
- Ruling: research phase first (P1 code review of platform/pipeline/production gaps, P2 literature on combination,
  construction with costs, risk models, capacity, platform design, new families) -> synthesis plan -> 3-4 implementer lanes
  -- owner asked for review + research before implementation -- cost if wrong: ~1 h of agent time before code moves.
- Ruling: research agents read-only, write to this dir; no measurements in this phase -- nothing to bias.

## 2026-09-28 P1 landed; root A5 started
- P1 code-review-v7.md (395 lines) delivered. Headline: v6.1 ladder = 234 s compute inside a ~90 min cycle; SHA-256 is
  30/76 s of u and 19/31 s of w; one new field invalidates 35/39 cache entries; research exes are Debug; no daily decide
  path; risk model + QP + GP aim exist unused in atx-engine/risk. Lanes proposed: L1 IC cache keys + fast hash, L2
  research-cycle driver + field reuse + fit store, L3 daily decide path (holdings emit, decide verb, deploy manifest),
  L4 DSL ops. Root: A5 Release A/B, C3 canary, disk GC.
- Ruling: A5 = build atx-equity-strategy-ic/targets under preset equity-rel (tag v7-rel0), then A/B the v6.1 u pass and
  the final NAV cell for byte identity; adopt Release only if identical -- identity re-run of an existing book is not a
  new trial (same book, same window) -- cost if wrong: one wasted build + two 180 s runs.
