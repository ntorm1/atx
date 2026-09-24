# Lane 9 (l9-realmine) status — branch feat/qps-l9-realmine (worktree C:\atx-wt\pool-10)

## State: REVIEW-FIX DONE (head c8d7702c; guarded rerun published)

## Review fixes (round 1)
- F1 adjustment breaks: ReturnGuard in score_signal (pnl + every IC horizon). One-day adjusted return
  excluded when |log| > 1.5 or adjusted |log| > raw_close |log| + 0.10. Verified the reviewer's case:
  69872 adj 0.3413 -> 29.987 on 2017-03-15, raw 49.20 -> 49.64. Per-role counts + examples in
  gate_report.json (train/validation/holdout .return_guard); per-score excluded_return_terms.
- F2 holdout discipline: --holdout off|publish (default off, holdout contexts never loaded),
  --holdout-prior-reads N -> report holdout.status fresh|reused. DISCLOSURE: 2019 was loaded by
  scratchpad l9_smoke1 (no score rows), scored by l9_smoke2 (family blend net 1.69, p 0.050) and by
  the published run equity_mine_l9_20260923 (1.82, p 0.052). It is a REUSED holdout; the rerun passes
  --holdout-prior-reads 3.
- F3 role leakage: build_role refuses a stitched span with sessions >= next role start (exit 1 +
  failure.json; stage errors are exit 1 by design, parse errors 2).
- Tests: 34/34 lane (+4 new: EquityMineGuard x2, CLI holdout-off, CLI leak refusal); full
  atx-impl-tests 515 pass / 5 skipped (repo-root-only) excluding known StageRunSyntheticSmoke.
- Rerun: scratchpad l9fix_real_run.ps1 -> C:/atx\data\equity_mine_l9_guard_20260923 (done, exit 0, wall 878 s, peak 0.96 GB).

## Guarded rerun results (C:/atx/data/equity_mine_l9_guard_20260923) -- supersedes equity_mine_l9_20260923
- Guard exclusions (span / realized-window): train 17/14, validation 13/4, holdout 4/0.
  Includes 69872 2017-03-15 (+8687% adj vs +0.9% raw), 3572541/3572568 2015-09-10, 3572601, 787527.
- 378 seeds, 2243 candidates, 2065 scored, N_eff 5.71, family 52 (1099 corr-rejected), admitted 0.
- Family val: mean net Sharpe -0.48, 10/52 positive, min raw p 0.18 -> BY p 1.
- Family blend VALIDATION: net Sharpe -1.22 (was +0.60 unguarded), gross -0.13, p 0.95. The earlier
  +0.60 / +6.1 bps/day was the 69872 adjustment break, as the reviewer said. Not admitted.
- Family blend 2019 holdout (REUSED, 4th load / 3rd score): net 1.73, gross 2.30, p 0.062. Descriptive
  only; contradicts validation, so no claim.
- Conclusion: no real alpha from this DSL family at 5 bps daily-rebalanced rank L/S on t1000 2013-18.

## Next
- Residualize vs size/liquidity before scoring (winners are adv/cap proxies); turnover-aware books;
  SearchDriver fitness hook to use honest net scorer; signal-side guard (adj close feeds adv/cap).



## What exists
- Stage `equity-mine` (atx-impl/src/stage_equity_mine.{hpp,cpp}); dispatch.cpp early-route statement
  (own argv parser, config.cpp untouched); one `target_sources` line in atx-impl/CMakeLists.txt.
- Pipeline: stitch yearly identified contexts -> train/validation/holdout span panels (bitwise overlap
  check), as-of PIT membership mask (membership.bin cut 0 = top1000/band0), alpha101 augmentation,
  SearchDriver (fidelity racing + semantic canon + output dedup) on TRAIN only, honest delay-1 rank-L/S
  net-of-cost re-score of every candidate -> durable TrialRegistry (N_eff, registry-fed DSR),
  SketchIndex corr-dedup validation family, BY + Romano-Wolf gate on validation, pre-registered family
  equal-weight blend as hypothesis K+1, holdout once, hash-bound manifest written last, failure.json only
  in a dir the stage created, refuses sessions >= seal.

## Commits
- 14015044 stage + dispatch + CMake + 3 test files
- 4c2d3c34 prefixed CSV headers + --smooth-windows decay variants
- d0ac9d62 family blend hypothesis K+1
- 60409f3d literature seeds lead search population

## Tests (equity-dev)
- `atx-build.ps1 -Ctest -Preset equity-dev -R '^(EquityMine[A-Za-z]*|AtxImplCli)\.'` -> 30/30 pass
  (22 EquityMine*: core scorer/stitch/as-of mask/parse, pipeline planted-recovered + null <=3/12 runs,
  CLI manifest hashes/seal/existing-out/smoothing; 8 AtxImplCli regression).

## Real run (Release build-equity-rel; needs -DFETCHCONTENT_BASE_DIR=C:/atx-wt/pool-10/deps/equity-rel)
- Out: C:\atx\data\equity_mine_l9_20260923\  wall 1174 s, peak working set 0.95 GB.
- Script: scratchpad l9_real_run.ps1 (train ctx 2013-16, val ctx 2016-18 window 2017-18, holdout ctx
  2018-19 window 2019; pop 192 x 15 gens; cost 5 bps; smooth 5;10; BY q=0.10).
- 378 seeds (25 lit + 101 WQ101 x {raw,decay5,decay10}), 2243 candidates, 2065 scored trials,
  N_eff 5.57, family 53 (1080 rejected by |corr|>=0.7), admitted 0, family blend not admitted.
- Train-best (net Sharpe 1.0-1.5, mostly search-found size/liquidity adv proxies) collapse on validation:
  mean val net Sharpe -0.33, 12/53 positive, min raw p 0.091 -> BY p = 1.
- Family blend: val net Sharpe 0.60 (p 0.20, p_by 1.0); holdout 2019 (reported once) net 1.82, gross 2.40,
  turnover 0.09, t_NW 1.62, p 0.052. Not admitted — descriptive only.

## Next (deferred)
- see final report deferred list.
