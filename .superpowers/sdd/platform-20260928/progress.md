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
