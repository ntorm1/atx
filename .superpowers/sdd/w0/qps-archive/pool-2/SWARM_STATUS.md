# Lane 1 (l1-kernels) status — Rolling/CS kernels + StreamingEngine

- Worktree `C:/atx-wt/pool-2`, branch `feat/qps-l1-kernels`, base `334a7939`
- Builder: DONE for this wave (2026-09-23). Not merged. Head: see `git log -1`.

## Commits (lane)
- 4b4e9726 radix rank, sliding order-stat, O(1) sliding windows (+ CsRadixRank_*, TsOrderStat_*, TsSliding*)
- 3d965ece WIP checkpoint: StreamingEngine + test, TsvWelfordState refactor, branchless order-stat counts, bench
  -> VERIFIED: builds clean, alpha tests green (StreamingEngine_* bit-exact AuditExact+ResearchFast, 44-alpha WQ battery)
- 02c53ce5 bench: BM_StreamingStep (lookback 60 vs 250, both modes)
- 959fdf21 cs_radix: MSD bucket split + insertion finish (+2 shape-pin tests)
- 5d17d9ba ts_sliding: steady-state fast path for linear-decay lane

## Tests
- atx-engine-alpha-tests (equity-dev): 646/646 PASS
- ctest -R lane+guard suites (CsRadixRank_, TsOrderStat_, TsSliding, StreamingEngine_, AlphaVm_Differential, AlphaConformance, TsWelford, TsEvalMode): 49/49 PASS

## Bench (equity-rel, box ~85% loaded by other lanes -> absolute ns inflated ~1.5-2x)
- rank(3000): kernel 23-27 ns/cell (stable_sort ref 73-98, 3.6x); engine 26-34
- ts_rank(20): engine 30, kernel 18 (batch ref 137)
- decay_linear(20) fast: engine 25, row-sweep kernel 12 (audit 61)
- corr(20): row-sweep kernel 38 vs batch 152 — NOT reachable via VM (vm.hpp owned by Lane 2)
- StreamingStep (5 alphas x 3000 names): 0.35-0.5 ms/alpha-day; lookback 250 vs 60 +4% fast / +11% audit (noise; state is O(d))

## Deferred / integration
- VM wiring (Lane 2 owns vm.hpp): ResearchFast pair ops -> sliding::sweep_comoment; unary sliding ops -> sliding::sweep_unary over [0,I) date-outer
  (then StreamingEngine needs a CoMoment TsKind for pair ops in ResearchFast to stay bit-exact)
- engine floor (~8-13 ns/cell LoadField copy + result alloc) is Lane 2 (zero-copy LoadField)
- L1 -> live path: loop/signal_source.hpp swap VmSignalSource for StreamingEngine
- shared C:\atx-cache\deps spdlog-build races between Debug/Release trees (_ITERATOR_DEBUG_LEVEL link mismatch); retry build

## Review-fix wave (2026-09-23 evening) — DONE, committed
- d70be43b ts_sliding: drift-triggered re-centre (kDriftRatio=1e-6 vs PEAK raw shifted 2nd moment since last re-centre) for CoMoment + TimeReg lanes. Fixes level-jump cancellation (10->1e5 and back, 1e-5 rel noise): before, corr off ~3e-6, rsquare off 5e-8 / clamped to 1 when the last pre-jump value left a window re-centred across the jump. LinDecay stays schedule-only (first-moment output; pinned by test).
  New tests TsSlidingCoMoment_LevelJump / TsSlidingUnary_LevelJump, d in {5,20,60} (red before, green after).
- 4949c2d1 StreamingEngine_Corpus101: all 101 canonical formulas (atx-impl/tests/fixtures/alpha101.txt, augmented panel via with_alpha101_fields) x {AuditExact, ResearchFast} x {24, 128 names (radix path)}: 101/101 stream bit-exact, 0 NotImplemented (pinned), non-vacuity floor >=85 alphas with finite cells (observed 97 / 90).
- Tests: atx-engine-alpha-tests 656/656 PASS; ctest lane+guard regex 70/70 PASS.
- Bench (equity-rel, after fix, box loaded): comoment sliding kernel corr 19-21 ns/cell, cov 15-20; unary sliding decay 7-9, slope 10-15. No regression vs earlier wave numbers (A/B checkout was not permitted; compared to prior report).
- Finding 1 (engine-path targets) NOT fixable in-lane: vm.hpp is Lane 2's. Integration dependency recorded (route ResearchFast TsCorr/TsCov/TsRegression -> sliding::sweep_comoment over [0,I); unary sliding -> one sweep_unary over [0,I); then add CoMoment TsKind to StreamingEngine; re-run corr/ts_rank/decay/rank engine benches before claiming acceptance).
- Finding 4 (3d965ece "WIP (unverified)" message): not rewritten — sits below sync merge 87c13ce9 on the shared lane branch; rewording needs a merge-preserving rebase + force push. Squash or reword at integration.
- Next: nothing pending in lane; await Lane 2 vm.hpp wiring.