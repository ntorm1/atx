# atx-vol Served-Surface No-Arb Integrity — SDD Progress

Controller: Claude Opus 4.8
Repo root: C:\atx  (atx-vol sources under atx-vol/)
Branch: feat/atx-vol-carry-deam
Plan: docs/superpowers/plans/2026-07-07-atx-vol-surface-noarb-integrity.md
Base commit at start: 1efeefb

## Task ledger
(none complete yet)

## Minor findings roll-up (for final review triage)
(none yet)

Task 1: complete (commits 1efeefb..527d728, review clean — Approved)
  Minor (roll-up): arb_test.cpp new tests cover only crossing+monotone; the 4 guard
  branches (<2 slices, n_grid==0, !(k_max>k_min), non-finite skip) lack direct coverage.

Task 2: complete (commits 527d728..2379606, review clean — Approved)
  Baseline: SPY dense board n_calendar_viol_pre=372, calendar_arb_free=false (pre-enforcement; Task 5 closes this).
  Minor (roll-up): session.cpp:284 `n_viol = cal ? cal->size() : 0` treats a failed check as
  0 violations / arb-free — opposite of sibling session.cpp:639 conservative `cal.has_value() && cal->empty()`.
  Currently dead (arb_check_calendar has no Err path). Fix for honesty: match sibling guard or add comment.

TEST OVERHAUL: complete (commit bd269f8, review clean — Approved)
  715s serial -> 111s parallel (6.4x). Standard fast gate: `ctest --test-dir build -L atx_vol -j16 --output-on-failure`.
  Benches (Corpus.Throughput_FitsUnderCeiling, BacktestBench.*StepsPerSecond) gated behind env ATX_VOL_BENCH; skip by default.
  Trims: PnlGreeks Session_ConvexDense 134s->47s (expiry subset); AndersenLake VsPdeOracle grids 30s->11s (5x3x3->3x2x2 corners).
  De-flakes: IngestScale timing asserts stripped (correctness kept); backtest_real PID-private temp dir for -j safety.
  bench_gate.hpp uses _dupenv_s under _MSC_VER (/WX). NOTE: two db-suite commits (a351506,f27ac2e) in range are a DIFFERENT agent's work, not ours.
  Minor (roll-up): unguarded <process.h> in backtest_real_test.cpp; stale IngestScale test names ("Linear" no longer asserted); PnlGreeks "back" sample stops ~5/6*N.

=== PIVOT 2026-07-07 ~5:30pm: user redirected to build a SOTA work module ===
Built: docs/superpowers/plans/2026-07-07-atx-vol-sota-engine-workmodule.md
  8-sprint ladder (A current no-arb integrity; B curve breadth C8/CStar; C cached greeks all kinds;
  D unified risk engine; E deep-wing strict no-arb; F native discrete-div PDE + rate bootstrap;
  G SoA quote book + perf-regression CI gate; H cross-method oracle + property/fuzz). Mission-locked to
  American-equity MM/HFT analytics (exotics/stoch-vol/MC/GPU explicitly OUT).
State: RESUMED 2026-07-07 ~5:45pm — user set new /goal: implement the SOTA work module via SDD.
  Sprint A (no-arb integrity) is in flight. Sprint A Tasks 1-2 + test overhaul DONE (commits
  527d728, 2379606, bd269f8, all reviewed clean). Sprint A remaining after Task 3 = A4-A8 in
  docs/superpowers/plans/2026-07-07-atx-vol-surface-noarb-integrity.md (execution-ready; briefs task-4..8-brief.md exist).

Task 3: complete (commit 4cab210 on base 38d0d97, review clean — SPEC ✅ / Approved)
  Augmented dense QP `qp_active_set` from homogeneous `Gx>=0` to `Gx>=h` (ratio-test residual now
  subtracts per-row h(i); h=0 recovers prior solver bit-for-bit — existing DenseSlice.* tests are the
  equivalence guard). fit_convex_slice now honors bound_slope_below via slope-below rows
  (g_{j+1}-g_j >= -df*(u_{j+1}-u_j)). New test SlopeBelowBoundHonored (failed pre-impl for right reason).
  Fast gate 680/680 pass.
  Minor (roll-up): (a) dense_slice.cpp:13 black76.hpp include is DEAD — PRE-EXISTING before A3 (no
  black76_price/value_and_vega calls in 38d0d97 pre-image); clean up opportunistically / at final review.
  (b) dense_slice_test.cpp helper relies on transitive <cmath>/<algorithm> (std::fabs/std::max) — add
  explicit includes for robustness. (c) rows.reserve(4*N) slightly over-reserves in default no-bound path.
  CARRY-FORWARD (A4/A5 + whole-branch): x0 strict-feasibility. The primal active-set assumes a strictly
  feasible start (G x0 > h). The new slope-below rows can make the unchanged quadratic x0 infeasible; it
  currently self-corrects via the existing KKT-drop/negative-alpha path and converges for all tested cases
  (680-gate green), but textbook convergence-from-infeasible-start isn't guaranteed and that robustness
  lives in UNCHANGED active-set code. A4 feeds a NON-ZERO per-node calendar floor h (larger displacement
  from x0) so this becomes materially more pressing there — A4 must ensure/verify a feasible seed
  (Phase-1 / project x0 onto floor) or confirm the fallback still converges with the calendar floor.

Task 4: complete (commit 6a80c3f on base 4cab210, review clean — SPEC ✅ / Approved)
  Per-node calendar floor in fit_convex_slice: optional w_prev(k) callback; where set, adds diagonal rows
  g_j >= black76_price(F,u_j,T,sqrt(w_prev(k_j)/T),df,Call) (guarded on finite&positive w_prev AND price).
  x0 seed lifted via pointwise-max against the floor curve + strict margin (RESOLVES the A3 x0-feasibility
  carry-forward for the non-zero-h case; keeps start convex/decreasing/positive & strictly feasible).
  w_prev defaults to {} => floor block skipped => bit-identical to pre-A4 (all 4 callers: vol_curve.cpp:165
  + 3 examples spy_bidask_bench/spy_dec_curve/spy_oos_check, all <=5-arg, unaffected). black76.hpp include
  now USED (resolves A3 roll-up (a) dead-include). Fast gate 682/682.
  PLAN DEVIATION (adjudicated ACCEPTABLE by controller + opus reviewer): brief's verbatim
  CalendarFloorSlackIsBitIdentical used make_synthetic_slice_obs(...,0.30), whose deliberately-cheapened
  deep-ITM K=65 print (mid*=0.5, tight spread, ~40000x weight) drives the UNCONSTRAINED fit below intrinsic
  at K=40/55 (free=57.58<58.8; 43.65<44.1) — and a Black-76 call at any positive vol is >= intrinsic, so
  ANY positive-vol floor binds there => "slack" premise mathematically unsatisfiable for that fixture.
  Implementer swapped ONLY that test's board to a clean strike_grid+mk_obs(0.30) board with w_prev=0.10^2*T
  (floor present but genuinely slack) => 1e-12 bit-identity is real/non-tautological. make_synthetic_slice_obs
  itself UNCHANGED (A3's SlopeBelowBoundHonored still depends on it). Lift test unchanged (active floor).
  Minor (roll-up): (a) rows/hrows reserve(4*N) can be exceeded when bound_slope_below AND full floor both
  active (up to 5N-4 rows) -> one realloc; bump to 5*N (hint only, correctness unaffected). (b) x0
  pointwise-max may sit exactly on a monotone/convex row (upward/convex kink only; fallback recovers) — no action.

Task 5: IMPLEMENTED but NOT COMMITTED — BLOCKED on a product decision (uncommitted changes live in the
  working tree, coherent + building clean; do NOT `git checkout`/discard them).
  Threading done: fit_slice_curve gains w_prev (forwarded to fit_convex_slice on ConvexDense; Essvi/Svi ignore);
  curve_fit.cpp driver feeds prev slice's w(k) as next slice's floor, guarded on out.context.back().T < T
  (ascending-T; loader sorts via data.cpp sort_chains_by_T). New test tests/curve_noarb_test.cpp mirrors
  spy_real fixture, repair=None, arb_check_calendar(surface,-0.6,0.6,64).
  RESULT: SPY dense calendar violations 372 -> 2 (NOT 0). Residual N=2 = between-node crossings surviving the
  64-pt grid (node-only enforcement); worst k=-0.4688 T 0.362->0.400 slack 2.88e-2; k=-0.4313 T 0.458->0.485
  slack 2.15e-2 (put wing ~0.4y). NOT masked (test asserts the measured baseline w/ KNOWN-RESIDUAL comment).
  BLOCKING REGRESSION: SPY has REAL fixed-k put-wing calendar crossings => floor is genuinely ACTIVE (not slack).
  On the default Mid loss the lift pulls fitted px off mid: session.cpp routes ConvexDense via fit_curve_surface,
  so px_clean 99.49% -> 94.65% (vs stashed clean baseline) => breaks all 3 SpyBidAskRegression.*Maintains99.
  Vega-weighted SpyRealOpra still passes. Implementer recommends: land Task 7 (Interval loss => floor lifts only
  to band edge, not mid) BEFORE enabling enforcement, OR accept + rebaseline. ESCALATED to user (AskUserQuestion)
  — flagship 99.49% metric tradeoff, plan sequences A7 after A5 and behind a flag, 3 defensible paths.
  USER DECISION: "Interval-loss first, then enforce." => REORDER: land Task 7 (interval loss) BEFORE A5's
  landing, then enable interval loss on the served ConvexDense path WITH A5 calendar enforcement, and require
  BOTH calendar-arb-free (~0/low residual) AND in-band px_clean >= 99.49% (no regression). If interval loss
  can't recover to >=99%, return to user. A5 stays UNCOMMITTED in the tree meanwhile (files listed above;
  do NOT discard). A7 and A5 touch DISJOINT files (A7: dense_slice.*, dense_slice_test; A5: vol_curve/curve_fit/
  curve_noarb_test/CMakeLists) so A7 lands cleanly on HEAD via explicit-path staging.

Task 7 (reordered ahead of A5 landing): IN FLIGHT. Interval (band) loss via slack vars z=[g;s+;s-] in
  fit_convex_slice behind ConvexFitOpts.loss{Mid} flag; interval branch REUSES A3 slope + A4 calendar-floor
  rows on the g-block (padded) so it composes with enforcement. Default Mid byte-identical. Base HEAD=4b89fc1
  (concurrent db agent merged feat/pf4-s2 — atx-impl only, 0 atx-vol files; our A4=6a80c3f is in history).
  A7 commit 62d7597 (3 dense_slice files only). NOTE: A7 agent stalled once (stashed its 3 files for a diagnostic,
  left empty diff); resumed via SendMessage -> popped/committed. Two numerical fixes it applied: (a) Hz *= 1/w_mean
  uniform rescale to balance KKT (REVIEW-CONFIRMED pure conditioning: scalar*H with q=0 => minimizer invariant);
  (b) wideband test board = convex quadratic.
  A7 REVIEW (opus): SPEC ❌ / QUALITY Approved. Implementation fully compliant (composition w/ A3+A4 rows verified —
  none dropped; Mid byte-identical @1e-12; band signs + feasible start correct; Hz rescale safe). DEFECT (Important):
  IntervalLossPutsPriceInsideBand is NON-DISCRIMINATING — for that board N==M => B=identity + mids on exact convex
  quadratic => g=co is global min for BOTH Mid and Interval => test passes verbatim with Mid opts (smoke test, not
  proof interval loss works). Unlike A4 (genuinely unsatisfiable), a discriminating test IS achievable. -> FIX
  dispatched: strengthen test to assert Mid lands >=1 obs OUT-of-band while Interval stays in-band (test-only, new commit).
  Minor (roll-up): interval branch recomputes the 3rd-diff roughness block (4-line dup of Mid H lines ~298-308) — keep
  in sync or hoist to a shared helper.
  A7 FIX (commit 918f125, test-only): strengthened IntervalLossPutsPriceInsideBand to discriminate — liveness guard
  ASSERT_GT(mid_violations,0) (default Mid fit overshoots 4 bands K=135-150 by up to 1.87) paired with Interval in-band
  on all 11 strikes @1e-6. Teeth proven by forcing Interval->Mid => test fails. make_synthetic_slice_obs untouched.
  Controller light-verified the test source directly (not a full re-review — test-only, self-checking, on-point).
Task 7: COMPLETE (commits 62d7597 + 918f125, review clean after fix — SPEC ✅ / Approved). Interval loss available
  behind ConvexFitOpts.loss flag, default Mid. Ready to enable on the served path in the integration step.

A5+A7 INTEGRATION (in flight): DESIGN = couple interval loss to floor enforcement IN THE DRIVER (curve_fit.cpp,
  A5-uncommitted): when a slice gets a w_prev floor (T beyond front usable expiry), fit it with interval loss
  (slice_cfg = cfg; if (w_prev) slice_cfg.convex.loss = Interval); the unfloored FRONT slice stays Mid (pristine).
  Rationale: interval loss lets the calendar-floor lift ride to the band edge, not off mid => no in-band regression.
  This needs NO scattered call-site/test-config edits — curve_noarb_test (default cfg, floor always-on for T>front)
  and SpyBidAskRegression (served via session->fit_curve_surface) exercise it automatically. Bench/non-enforced
  ConvexDense paths keep Mid. Seam confirmed: served path session.cpp:273 fit_curve_surface(under,sp,eff.curve);
  eff=SessionInputs, loss lives in curve.convex.loss (SessionInputs.curve default Essvi; callers opt into ConvexDense).
  A5 LANDING = the full A5 threading + this coupling committed together once measured green. ACCEPTANCE (objective):
  3 SpyBidAskRegression.*Maintains99 PASS + SpyRealOpra PASS + curve_noarb residual <= 2 (ideally 0, re-measured under
  interval since interval changes the fitted curve) + fast gate green. Report exact px_clean (Mid was 94.65%, headline
  99.49%). If in-band tests fail or residual worsens => do NOT commit, escalate.

*** PERF BUG FOUND + FIXED (user: "fits should not materially slow when adding arb constraints") ***
ROOT CAUSE: interval loss added 2M SLACK VARIABLES (z=[g(N);s+(M);s-(M)]). qp_active_set (dense_slice.cpp:42)
  rebuilds + DENSELY factorizes a (n+nw)x(n+nw) KKT EVERY iteration (solve(K,rhs), line 76). Interval made
  n=N+2M (~440 for SPY M~200) vs N~40 => ~(440/40)^3 ~1000x per-iter blowup + far more active-set iters as the
  2M band constraints activate. Calendar floor (A4) alone is CHEAP (adds ROWS, not vars). Also killed a ZOMBIE
  atx-vol-tests.exe (PID 22868) — a full-board fit that survived TaskStop of its wrapper and kept churning CPU.
FIX (keeps QP N-dimensional => fast):
  1. curve_fit.cpp: REVERTED the interval-loss coupling (back to fit_slice_curve(cfg,...); no slice_cfg override).
  2. dense_slice.cpp: CAP the calendar floor at the ask envelope — cfloor_j = min(black76(w_prev@node), ask@node),
     ask interpolated from merged obs (co(i)+so(i)/2; added so(M) extraction of Node.s). Floor lifts a node only to
     its tradeable bid-ask edge => stays ~in-band, no slack vars. A7 interval loss stays committed as an unused
     opt-in flag (off by default). dense unit tests now 35ms (were minutes); all 10 ConvexSliceFit/DenseSlice PASS.
  3. Test: renamed CalendarFloorLiftsLowVarianceSlice -> CalendarFloorLiftsButStaysInBand; asserts floor ACTIVE
     (floored iv > free iv) AND CAPPED (floored iv < 0.25 uncapped target). CAVEAT: cap is best-effort in-band, NOT
     a hard per-obs guarantee (convexity/monotonicity can drag the tail a hair over an ask on pathological boards);
     aggregate in-band is the SpyBidAskRegression gate. Measuring SPY now (residual/in-band/wall-clock, fast QP).
  Files touched (uncommitted, part of A5 landing): curve_fit.cpp, dense_slice.cpp, dense_slice_test.cpp (+ A5's
  vol_curve.*, curve_noarb_test.cpp, CMakeLists). curve_noarb_test residual constant (2, Mid) must be RE-MEASURED
  under the capped floor before commit.

MEASURED TRADEOFF (fast QP, SPY board): no-enforce 99.49%/372resid | strict-floor+Mid 94.65%/2 |
  capped-floor+Mid 97.51%/156 | interval SLOW. Capped floor was a muddy middle (neither tight nor clean).
  Finding: on SPY the front slices carry GENUINE calendar structure => enforcement CANNOT be free; it trades
  ~2-5pp in-band. Plan premise ("enforcement mostly slack/free") is empirically FALSE for SPY.
USER DECISION #2: "Enforce strict floor by default" (94.65% in-band, 2 residual; rebaseline SpyBidAsk).
  => REVERTED the ask-cap experiment entirely: git checkout HEAD -- dense_slice.cpp dense_slice_test.cpp
     (back to committed A4 strict floor + A7 interval-as-opt-in-flag). A7 interval loss stays committed, UNUSED
     on the hot path (off by default; documented as opt-in). Cap code fully removed.
  => A5 driver (curve_fit.cpp) feeds STRICT w_prev floor on floored slices (ascending-T guard), always-on.
     Comment updated to strict-floor + "rows not slack vars => no material slowdown".
  => Rebaselined spy_bidask_regression_test: kPxCleanFloor 99.0 -> 94.0 (strict floor measures 94.65% deterministic);
     renamed ConvexDense*Maintains99 -> *InBand, PricerFitter*Maintains99 -> *InBand; header + floor comments
     document the deliberate ~4.8pp calendar-enforcement tradeoff (MM product choice, not a regression).
  => curve_noarb_test kKnownResidualCrossings=2 unchanged (strict floor gives 2, confirmed).
A5 LANDING FILES (uncommitted, staging on gate-green): src/vol_curve.cpp, include/atx/vol/vol_curve.hpp,
  src/curve_fit.cpp, tests/curve_noarb_test.cpp, tests/CMakeLists.txt, tests/spy_bidask_regression_test.cpp.
  (dense_slice.cpp/.hpp/dense_slice_test.cpp are CLEAN — no A5 change there.) Running full fast gate before commit.

A5 LANDING COMMITTED: 5b46b74 "enforce calendar no-arb by construction on the served dense surface" (7 files,
  +198/-25). Fast gate: 685 tests, first run 684/685 (only SpyArchiveRoundTrip failed on its OWN un-rebaselined
  kPxCleanFloor=99 -> also rebaselined to 94; round-trip FIDELITY bit-identical, only accuracy threshold moved).
  Confirmed individually: dense unit 10/10 (35ms), SpyBidAsk 2x InBand PASS, SpyArchiveRoundTrip PASS (94.65%),
  CurveSurfaceNoArb residual 2 PASS. No other 99-gates in the suite (grep). SPEED: full gate 94s (was going to be
  >7min/test under interval). A7 interval loss remains committed as an unused opt-in flag (ConvexFitOpts.loss, Mid
  default). IN FLIGHT: confirming full gate (bjuufwc5i) + A5-landing task review (base 918f125..5b46b74).
Task 5 (+ A5+A7 integration): COMPLETE — strict-floor landing 5b46b74, review clean (SPEC correct / Approved, opus)
  + confirming full gate 100% (685/685, 140s). Interval-first explored, reverted for perf; user chose strict-floor
  default. A7 interval loss stays committed as unused opt-in flag. Minor (roll-up): curve_noarb_test.cpp:305 `64`->`64u`
  (cosmetic, not /WX). TEST-PERF ITEM (Sprint G): curve_noarb_test ~94s is the gate bottleneck (140s vs 111s pre-A5);
  cold-Andersen-Lake-per-strike full-board, like the SpyBidAsk/SpyArchive/PnlGreeks family — trim/cache as a family,
  not a one-off (residual sits at ~0.4y so a naive subset changes it).
SPRINT A REMAINING: A6 (Lee wing extrapolation), A8 (end-to-end verification). Base for A6 = HEAD 5b46b74.

A6 RESULT: BLOCKED (correct dense-slice impl, but finite Lee wings expose ~50 wing calendar crossings that A5's
  node-only floor doesn't couple; arb_check_calendar previously SKIPPED the NaN wings => baseline 2). Implementer
  refused to mask/commit-red (correct). A6 DEFERRED into Sprint E (deep-wing strict no-arb + wing-aware calendar
  enforcement belong together). A6 patch saved: <scratchpad>/a6-lee-wings.patch + a6-lee-wings-report.md; tree
  reverted clean to 5b46b74; rebuilt.
=== PAUSE 2026-07-07 ~9:45pm: user asked to build a next-sprint handoff plan and STOP (no implement). ===
Built: docs/superpowers/plans/2026-07-07-atx-vol-noarb-followups.md — captures Sprint A close-out (R1=A8 e2e),
  R2 (A6 Lee wings + wing-aware calendar enforcement -> Sprint E), R3 (between-node residual 2->0, grid-aligned
  floor), R4 (SPY cold-AL test-perf pass -> Sprint G), all Minor roll-ups, key findings (naive QP solver: constraints
  as ROWS not VARS; SPY enforcement not free ~4.8pp; strict-floor default), and the whole-branch-review/finish steps.
STATE: PAUSED per user after building the handoff plan. HEAD 5b46b74, tree clean, suite 685/685 green.

=== PF2 WORK MODULE (QIS vega-flat dispersion northstar) — SDD execution START 2026-07-08 ~7:30pm ===
Plan: atx-vol/sprints/pf2/2026-07-08-atx-vol-qis-dispersion-northstar-workmodule.md
Base HEAD: 5b46b74.  Branch: feat/atx-vol-carry-deam.  Controller: Opus 4.8.
Sprint order: S0 -> S1 -> (S2->S3) || S4 || S5 -> S6.
  S6 is operator-gated (paid Databento pull) — STOP for explicit approval before S6-2.
Pre-flight plan scan: clean (no plan-vs-rubric conflict requiring a batched question).

BASELINE (user decision "fold into S0-1"): S0 entered with a DIRTY tree on purpose.
  Uncommitted prior-session fast-cold de-Am preset threading (calib.hpp/.cpp, curve_fit.cpp,
  curve_noarb_test.cpp; al_opts/iv_tol/iv_max_iter, nullopt/1e-7/64 defaults = bit-identical)
  FOLDS INTO the S0-1 commit. Controller reverted the atx-vol/CMakeLists.txt temp-probe hunk
  (fit_timing_probe, marked "do not commit"); fit_timing_probe.cpp left untracked/orphaned.
  EXCLUDE from every commit: atx-vol/CMakeLists.txt, examples/fit_timing_probe.cpp, deleted
  atx-vol/sprints/*.md, atx-impl/pf3 plan, repair-wmi.*, archive/research/**. Explicit-path staging only.

## Sprint S0 — Fit-path + test-suite perf  (acceptance: SPY family <= ~70s from 207s, bit-identical fits, no coverage loss)
S0-1: COMPLETE (commit 5b46b74..a5846e0, review clean — SPEC ✅ / Approved, sonnet).
  Hoisted parallel_for to include/atx/vol/parallel_for.hpp; fit_curve_surface split into a parallel
  per-chain de-Am pre-pass (run_deam_prepass) + unchanged sequential ascending-T fit walk; fit_workers
  on SurfaceParityInputs (0=hw concurrency, 1=serial). Bit-identical 1-vs-N (field-wise SliceContext +
  grid-sampled iv(k), synthetic + real SPY). value_chain bit-identical (verbatim hoist). Cold de-Am
  preserved (AmericanCorrectionCaches{}). Full gate 687/687, /WX clean. SPY fit speedup ~1.8x (Amdahl-
  capped by the still-sequential build_parity_data 2nd de-Am => S0-2 target). Prior-session preset work
  folded in per user decision. Commit surgical (9 allowlist files; noise still untracked, verified).
  ROLL-UP (for final whole-branch review):
    (Important, PRE-AUTHORIZED) folded preset threads al_fast_opts+iv_tol 1e-5 into the FIRST (fit-input)
      de-Am pass for Fast/Hft served ConvexDense (was hardcoded ACCURATE 1e-7/64). Cold preserved (plan's
      literal cache constraint held); in-band gate ConvexDenseServedViaSessionInBand 94.65% green. Wants
      explicit production-accuracy sign-off at final review (user already authorized fold-in).
    (Minor) parallel_for.hpp worker lambda has no try/catch — an escaping exception std::terminate()s;
      pre-existing verbatim hoist, but now 2 prod call sites; calib_pool wraps its fan-out. Harden centrally.
    (Minor) curve_selector.cpp:86 + 3 examples still call the 6-arg build_observations_european (old ACCURATE);
      widening asymmetry to watch if later tasks extend preset threading.
S0-2: COMPLETE (commit a5846e0..c61079b, review clean — SPEC ✅ / Approved, sonnet).
  bool score_parity{true} on SurfaceParityInputs; parity block wrapped in `if (in.score_parity)`
  (parity-ON line-for-line identical to history; fit/SliceContext/commit path untouched; parity-OFF
  zeroes per_expiry + worst_frac=0). Full gate 689/689, /WX clean. SPY fit parity-off speedup 4.15x.
  ROLL-UP (Minor): ParityOffMatchesParityOnSurface asserts per_expiry.size()==size() and n_slices==4
    but not per_expiry.size()==n_slices directly (true by construction). One-line strengthen optional.

MEASUREMENT (post S0-1+S0-2, -j1, build/): SPY family = 175.7s (was 207s). Per-test:
  SpyBidAsk.AutoSelectPicksDenseForSpy 39.6 | SpyBidAsk.PricerFitterExplicitConvexInBand 29.9 |
  CurveSurfaceNoArb.SpyDense 28.0 | SpyBidAsk.ConvexDenseServedViaSessionInBand 20.0 |
  SpyArchiveRoundTrip 19.0 | SpyRealCalendarReporting 12.2 | BacktestReal(2) 9.7+8.4 |
  SpyPortfolioPnl 4.8 | PnlGreeks.Session_ConvexDense 2.4 | PnlGreeks synthetic ~0.05 each.
  INSIGHT: fan-out (S0-1) already made pure-read fits cheap (PnlGreeks.Session 14.8->2.4, PortfolioPnl
  16.9->4.8). Residual is dominated by (a) the 4 FIT-ASSERTING tests = 117.5s (S0-4 subset target) and
  (b) the SEQUENTIAL second (parity) de-Am that parity-ON tests still pay — S0-1 fanned phase-1 ONLY,
  build_parity_data (phase-2, per-slice) is NOT fanned. gtest_discover_tests => 1 process/test, so an
  in-process shared fixture CANNOT amortize across the 5 read-only tests under ctest.
  => S0-3 RESCOPE (pending investigator map): fan out the parity/second de-Am in curve_fit.cpp (prod +
     parity-ON test win, sidesteps the cross-process fixture problem) + set score_parity=false in the
     read-only tests that do NOT assert parity/in-band. S0-4: subset the 4 fit-asserting boards
     (preserve noarb ~0.4y 2-crossing baseline + in-band floor).
S0-3 INVESTIGATOR MAP (cavecrew): NO SPY-family test asserts the re-Am parity diagnostic
  (frac_fv_within_bidask). Tests 1-2 assert CALENDAR arb-free (arb_check_calendar, independent of
  build_parity_data); tests 3-6 assert price_in_band (independent recompute); 7-10 greeks/roundtrip/
  tearsheet. AutoSelectPicksDenseForSpy (39.6s) fits TWO families (ConvexDense node_cap 40 + Essvi)
  with OOS CV per expiry (curve_selector.cpp:21-31,64-92). BacktestReal builds a 3-date corpus once in
  SetUpTestSuite then MarketSnapshot::load (cheap). So build_parity_data is DEAD COST for every SPY test,
  and the SEQUENTIAL phase-2 parity de-Am is the dominant residual on parity-ON fits.
S0-3 RESCOPED (fixture -> parity de-Am fan-out): fold build_parity_data into run_deam_prepass (parallel
  per-chain), phase-2 consumes precomputed ParityData. Bit-identical parity numbers; production win
  (served path is parity-ON); no API threading; concurrent correction-cache reads proven safe (S0-1
  review + value_chain precedent).
S0-3: COMPLETE (commit c61079b..86ba977, review clean — SPEC ✅ / Approved, sonnet). per_expiry
  bit-identical across worker counts (all 8 ParityReport fields, exact EXPECT_EQ); build_parity_data
  untouched; same args (F/q_eff). Full gate 689/689. Parity-ON SPY fit ~4x (8.5->2.1s quiescent).
  ROLL-UP (Minor cosmetic): expect_per_expiry_bit_identical lacks the ordering-assumption comment.

MEASUREMENT post S0-3 (-j1 SPY family): 61.6s (from 207s, 3.4x) — UNDER the <=70s S0 gate. Per-test:
  BacktestReal 11.2+15.4 | AutoSelect 10.5 | ArchiveRoundTrip 5.8 | PortfolioPnl 5.0 | SpyBidAsk 3.8/2.6 |
  Session_ConvexDense 2.7 | CurveSurfaceNoArb 2.2 | CalendarReporting 1.4. S0-3 parity fan-out was the
  dominant win (CurveSurfaceNoArb 28->2.2, SpyBidAsk 20->3.8, CalendarReporting 12->1.4).
S0-4 ORIGINAL (subset fit-asserting tests): SKIPPED — gate already met at -j1; fit-asserting tests now
  2-10s; subsetting risks the noarb ~0.4y 2-crossing baseline for negligible gain (YAGNI + risk).

OVERSUBSCRIPTION FINDING (from full-gate measurement): fan-out spawns hw_concurrency(16) threads/fit;
  under ctest -j16 => 16 procs x 16 threads = 256 on 16 cores => THRASH. Full gate: -j16=189.7s vs
  -j8=83.0s (2.3x faster at half ctest -j), vs pre-S0 -j16 ~140s. Documented -j16 command regressed 36%.
S0-4' (replaces S0-4): COMPLETE (commit 86ba977..aabf6cd, review clean — SPEC ✅ / Approved, sonnet).
  Inline atx_auto_worker_count() in parallel_for.hpp resolves auto(0) => ATX_VOL_FIT_WORKERS (positive int)
  else hardware_concurrency; used ONLY in the nt==0 branch (explicit counts uncapped); env read via
  _dupenv_s/_MSC_VER + std::getenv (bench_gate.hpp pattern); std::from_chars full-consume + >=1 parse
  (empty/0/non-numeric/neg/overflow => fallback). New parallel_for_test.cpp + FitBitIdenticalUnderEnvCap
  (RAII env guard). Full gate ATX_VOL_FIT_WORKERS=1 -j16 = 70.5s / 691 pass, /WX clean.
  ROLL-UP (Minor): empty-string env ("") not distinctly tested (Windows _putenv_s("","") deletes; matches
    brief recipe; from_chars empty-range path still correct, just untested on POSIX).

*** SPRINT S0 CLOSED (2026-07-08) — acceptance gate MET+EXCEEDED. HEAD aabf6cd. ***
  Gate: SPY family <=~70s from 207s => -j1 61.6s ✓; full fast gate ATX_VOL_FIT_WORKERS=1 -j16 = 70.5s
    (was ~140s pre-S0) ✓. Bit-identical fits across worker counts ✓ (S0-1/S0-3, exact EXPECT_EQ on
    surface iv + SliceContext + all ParityReport fields). No coverage loss (691 tests, +6 new, 0 removed).
    /WX clean; determinism preserved.
  Production served-fit win: single served ConvexDense fit ~8.5s -> ~2.1s (~4x) from phase-1 + phase-2
    de-Am fan-out (session.cpp:273 path; default fit_workers=0 => hardware_concurrency, parallel by default).
  4 commits: a5846e0 (S0-1 fan-out) c61079b (S0-2 parity opt-in) 86ba977 (S0-3 parity fan-out)
    aabf6cd (S0-4' env cap). Original S0-4 (subset) SKIPPED — gate met, subset risks noarb ~0.4y baseline.
  OPEN for final whole-branch review sign-off: (Important, pre-authorized) folded preset loosens the
    served fit's pass-1 de-Am AL tol 1e-7->1e-5 (cold preserved; in-band 94.65% green). Minor roll-ups:
    parallel_for.hpp worker lambda no try/catch; 6-arg build_observations_european call sites asymmetry;
    per_expiry ordering-assumption comment; empty-string env untested; S0-2 per_expiry==n_slices indirect.

## Sprint S1 — Multi-name pipeline correctness (NEXT). Depends on S0 (done). Base HEAD aabf6cd.
  Tasks: S1-1 distinct per-symbol uids across a corpus archive (THE northstar blocker — uid=1 collision
    fails SurfaceSet::create on any multi-symbol date); S1-2 symbol<->uid binding for DispersionUniverse;
    S1-3 drop-and-renormalize on unavailable name (MissingNamePolicy); S1-4 per-name fit-quality gate.
  Acceptance: multiname_pipeline_test — synthetic corpus (index + >=10 names, >=3 dates, >=1 thin, >=1
    missing) builds -> archives -> loads -> ATM-straddle dispersion backtest e2e, bit-identical across
    thread counts, per-name fit-quality + drop scoreboard. No paid pull.
S0-3: pending (shared cached SPY fit fixture for read-only tests).
S0-4: pending (subset fit-asserting tests, preserve ~0.4y noarb baseline).

### S1-1 DONE — commit c7721aa (distinct per-symbol uids across a corpus archive) — THE northstar blocker
  Implementer DEVIATED from brief (brief cited surface_archive.cpp:339 `plan.uid = ps.uid()`); scoped fix to
  corpus.cpp's write call site via `with_uid` instead. Review (sonnet, adversarial, deviation named as target):
  **Deviation verdict SOUND. Spec ✅. Task quality Approved. 0 Critical / 0 Important.**
  Reviewer independently proved the brief's line was INSUFFICIENT: write_surface_archive writes the DIRECTORY
  uid (de.uid :448), the lookup slot (s.uid :425) and the blob HEADER (bh.uid :519) all from `plan.uid`, BUT the
  blob-embedded ArchivePricingRecord.uid is written separately at surface_archive.cpp:466 from
  `to_pricing_record(plan.surf->pricing())` — i.e. straight off the original PricedSurface, not plan.uid.
  reconstruct() (:832-834,:918) rebuilds PricingContext ONLY from that embedded record; the directory uid is
  never applied. SurfaceSet::create keys on s->uid() (portfolio_pricer.cpp:142) = the BLOB uid, while
  MarketSnapshot::uid_of reads the DIRECTORY uid (backtest.cpp:194-212). Patching :339 alone leaves the blob at
  uid=1 => still "duplicate uid". with_uid at the corpus call site makes both storage locations read the same
  field of the same object => they provably agree. Also confirmed the "9 test files" claim (surface_archive_test
  hand-stamps uid 43 for "S00042", 100+i for "GROW0".."GROW7" — a shared write_surface_archive patch would have
  silently broken them). corpus.cpp is the single shared writer for every build_corpus caller.
  Back-compat verified by inspection + re-run: Corpus.RoundTrip_ReloadedSurfaceReproducesFreshFitBitIdentical,
  SpyArchiveRoundTrip.ConvexDense_Serialize_Reload_ReproducesTheoAndAccuracy, all Corpus.*, all BacktestReal.* pass.
  Commit surgical: 5 allowlist files, +305/-3. Full gate 693/693.
  ROLL-UPs (Minor, non-blocking, for whole-branch review):
    - universe.cpp:179 kSymbolCanonMax=32 vs surface_archive.hpp:78 kArchiveSymbolMax=32 duplicated with only a
      comment linking them; no static_assert. Unreachable today (Universe::kMaxTickerLen=16 caps it).
    - no test pins uid_for_symbol's truncation boundary (symbol >16/>32 chars) against the archive's truncation.
  NOTE: write_surface_archive guards duplicate SYMBOLS, not duplicate uids; a genuine FNV-32 collision therefore
  surfaces loudly at MarketSnapshot::load via SurfaceSet::create's duplicate-uid Err. Never silent corruption.

### S1-2 DONE — commit e94d303 (symbol<->uid binding for the dispersion universe)
  Review (sonnet, adversarial): **uid_for_symbol stability UNCHANGED. Spec ✅ (all 9 constraints).
  Task quality Approved. 0 Critical / 0 Important.**
  Built: dispersion.{hpp,cpp} `SymbolUidLookup` callable seam + `resolve_universe_uids` (module stays pure —
  no backtest.hpp include, verified transitively); universe.{hpp,cpp} expose `canonical_symbol` as single
  source of truth, uid_for_symbol refactored to hash it; static_assert(kSymbolCanonMax==kArchiveSymbolMax)
  at universe.cpp:40 (clears an S1-1 roll-up); backtest.cpp:212 uid_of canonicalizes its QUERY (widening-only);
  dispersion_strategy.cpp resolves in on_step/signals/build_book. dispersion_signal/build_dispersion_book
  signatures byte-identical to c7721aa. Fails loudly (empty/unknown/dup-symbol/dup-uid/uid==0) naming the symbol.
  KEY REFUTATION (the one defect that would have silently corrupted every archive): the old uid_for_symbol
  built a zero-init std::array<char,32> but hashed only `string_view(canon.data(), n)` — length n, NOT 32 —
  so it never hashed the padding. New code hashes an n-length std::string with the same FNV-1a32 offset basis,
  prime, `unsigned char` cast, and h==0=>1 sentinel. Reviewer independently re-derived FNV-1a32 in PowerShell
  with no reference to the source: SPY=1478221309 AAA=3061902210 BBB=2641672453 CCC=1716134816 spy=1478221309
  — exact match to UidForSymbolValuesArePinned (multiname_pipeline_test.cpp:343-350). Pins are real, not vacuous.
  Back-compat: strategy_test/tearsheet_test/examples author {IDX:1,NM0:2,NM1:3} and their test-local
  write_archive stamps ps.uid() (surface_archive.cpp:339, NOT uid_for_symbol), so resolution is a verified no-op.
  Commit surgical: 8 allowlist files, +358/-15. Full gate 698/698, /WX clean.
  ROLL-UPs (Minor, non-blocking):
    - dispersion_strategy.cpp:45 re-runs resolve_universe_uids in all 3 of on_step/signals/build_book
      (O(N^2) dup-uid scan + O(dir) linear uid_of per member, up to 3x per step). Immaterial <=100 names;
      cache one resolve per snapshot if baskets grow.
    - universe.hpp:101 uid_for_symbol is noexcept but now allocates via canonical_symbol (canon <=32B exceeds
      SSO ~15) => bad_alloc calls std::terminate. Sole production call site corpus.cpp:288, once per
      (date,symbol) board at write. Consider a fixed-buffer canonical_symbol overload. Reviewer VERIFIED the
      hypothesized per-directory-entry allocation in uid_of does NOT exist (allocates once per call, outside the loop).
  ⚠ INCIDENT (unrelated to the commit): the two untracked repo-root files `repair-wmi-for-python-pip.ps1`
  and `.log` vanished from the working tree during the S1-2 implementer run. Never tracked => unrecoverable
  from git; not in the commit. They were on the implementer's explicit NEVER-STAGE list. Surfaced to the user.

### S1-3 DONE — commits 9db4484 (S1-3) + d54c191 (S1-3a fix) + fed256e (S1-3b engine visibility)
  S1-3 review (sonnet): Gate PARTIALLY MET, Task quality NEEDS WORK. 1 Critical + 1 Important.
    CRITICAL: dispersion_strategy.cpp on_step cleared the book (d.clear) BEFORE the build that can fail
      into the no-trade path => a roll date with survivors < min_names FORCE-CLOSED the held basket
      instead of holding it flat. Untested because both S1-3 e2e tests put the missing name at INCEPTION.
    IMPORTANT: the plan's real gate (name absent on ONE date, held across it) was never tested, and the
      implementer's justification was FACTUALLY WRONG. Reviewer proved it: compute_step Errs only for a
      SETTLING lot; for an ALIVE lot portfolio_pricer.cpp:309-314 sets PriceStatus::ModelUnavailable and
      the reduction loop (:370-384) `continue`s past it BEFORE adding to f.total => the lot's PnL is
      SILENTLY EXCLUDED and pnl_explain returns Ok. Pre-existing engine defect, not introduced by S1-3.
  S1-3a (d54c191): moved d.clear to strictly after build_dispersion_book returns Ok (cohort_counter_++
    also below it). Added NoTradeOnRollDateLeavesBookIntact (RED: book came back empty) and
    HeldNameGoesMissingMidRunAndRunCompletes (pins the truncation: 2/8 legs ModelUnavailable, n_ok=6).
    Killed the tautological Sigma-w-hat assertion; added the empty-symbol authoring-bug case.
  S1-3b (fed256e): StepPnl.n_unpriced = alive.size()-n_ok; new BacktestResult::n_unpriced_lots column
    (both overloads, row0=0, TSV column table); RunConfig::unpriced = UnpricedLotPolicy{ExcludeAndReport
    (default = today's arithmetic), Error}. Fixed-book overload rewired onto compute_step.
    Engine truth: n_unpriced_lots is {0,2,2} not {0,2,0} — pnl_explain checks BOTH sb and st, so the
    d2->d3 step is ALSO unpriced (base=d2 lacks BBB). Implementer found this; brief's prediction was wrong.
  S1-3a+b review (sonnet, own-commit diffs only): Ordering-fix SOUND. Bit-identity PRESERVED (removed
    fixed-book inline loop is character-for-character identical to the pre-existing compute_step; strategy
    overload arithmetic zero diff). {0,2,2} correct. n_ok counts POSITIONS not contracts => the subtraction
    is sound even when two lots share a contract. Error policy fires with count+first uid, never for a
    settling lot, never on an empty book. **Task quality Approved. 0 Critical.**
  IMPORTANT (open, feeds S1-3c): book_greeks still silently under-counts gross_vega/gross_delta —
    PriceTotals::n_ok exists but run_backtest never reads it. And n_unpriced_lots is NOT a valid proxy:
    it measures a STEP's PnL completeness (needs the surface on BOTH base and shifted), while gross_* is a
    SINGLE-DATE snapshot. Reviewer traced the corpus: row 2 has n_unpriced_lots=2 while gross_vega on that
    same row is COMPLETE (book_greeks prices against d3 alone, where BBB is back). They diverge.
    => a "vega-flat" reading can be silently overstated. Fixing in S1-3c before S2 builds on it.
  ROLL-UPs (Minor): on_step's lot-opening loop can Err after the clear, leaving a half-cleared book
    (inert — every caller aborts the run); fed256e's commit message overstates the "full-column TSV diff"
    (tests pin ~8 of 24 columns); n_unpriced_lots absent from the 5 "every column" comparators
    (backtest_test expect_result_bit_identical, backtest_exec_test, backtest_real_test,
    spy_strangle_backtest_test, tearsheet_test dcols x2).

  *** BRANCH INCIDENT (not mine): a `git merge feat/pf4-s3` landed at 2026-07-09 20:12 (commit 57816da,
  parents 9db4484 + ad47252), between S1-3 and S1-3a, pulling 20 unrelated atx-pf4 Python data-warehouse
  commits onto feat/atx-vol-carry-deam. Reflog: "merge feat/pf4-s3: Merge made by the 'ort' strategy."
  Our commits sit cleanly on top; nothing of ours lost. Likely also explains the vanished untracked
  repair-wmi-for-python-pip.{ps1,log} and the FrictionMonotonicity -j16 timing flake (concurrent session
  in the same tree). NOT unwound — history rewrite on a shared branch is the operator's call.
  Consequence: SDD review packages must use own-commit diffs (git show <sha>), not BASE..HEAD ranges.


---

# atx-vol surface_db — SDD Progress (2026-07-11)

Controller session: surface_db feature
Worktree: c:/atx/.claude/worktrees/feat-atx-vol-surface-db
Branch: worktree-feat-atx-vol-surface-db
Plan: docs/superpowers/plans/2026-07-11-atx-vol-surface-db.md
Base commit at start: 4133e2d (plan commit; feature work starts after)

## Task ledger
(none complete yet)

## Minor findings roll-up (for final review triage)
(none yet)

## Environment notes (worktree bring-up)
- pwsh absent; use `& .\scriptstx-build.ps1 ...` from Windows PowerShell 5.1.
- Fresh-worktree configure: MUST `git submodule update --init --recursive` first (databento-cpp), and pin `-DCMAKE_MT=C:/Program Files (x86)/Windows Kits/10/bin/10.0.22000.0/x64/mt.exe` on the `cmake --preset ninja` call (find_program(mt) fails under this shell chain even though mt.exe is on PATH).
- Configure helper: scratchpad configure-worktree.cmd (vcvars64 -> Ninja PATH -> cmake preset ninja + CMAKE_MT pin).
Task 1: complete (commits 36ceebd..d9449e3, review clean - Approved)
  Minor (roll-up): CMakeLists src list placement of detail/archive_util.cpp not strictly alphabetical (list already unsorted; cosmetic).
Task 2: complete (commits 0ea8757..3b9eaa9, review clean after 1 fix loop - Approved)
  Fix loop: enum wire-range rejection test added (Open_RejectsOutOfRangeEnum) + writer doc reword.
  Minor (roll-up): test hardcodes record-interior offset +36 (could use offsetof(DbSymbolRecord, preset)); open() runs bounds sanity before header CRC (traced non-exploitable); DbPartitionRecord::flags write-0/never-read (future task).
Task 3: complete (commits 5795c7a..e0391cc, review clean - Approved, opus; 0 Critical/Important)
  Minor (roll-up): kSurfaceDbKeyMax doubles as symbol-canon truncation length in upsert/remove_symbol
  (name says "partition-key chars"; alias kSurfaceDbSymbolMax would remove ambiguity); persist_locked
  updated_ts_ns=0 "// now" semantics never asserted by a test; coverage gaps — empty-canonical-symbol
  InvalidArgument, refresh() IoError/ParseError branches, partitions surviving symbol mutation with
  non-empty partition set (last one blocked until Task 4 write_partition exists).
Task 4: complete (commits 3dbdf73..f695879, review clean - Approved, opus; 0 Critical/Important)
  Kind coverage verified against binding oracle: ConvexDense node byte-equal + LinearVariance k/w memcmp
  blocks pattern-identical to surface_archive_test.cpp; no kind switches in db path (delegates to archive).
  Minor (roll-up): archive-write happens BEFORE writer lock (brief-mandated; safe only under documented
  single-writer discipline — two same-key concurrent write_partition calls could pair one thread's manifest
  record with the other's file); stat/persist failure after successful archive rewrite leaves stale
  surface_count/file_size metadata for an existing key (opens fine; brief's ordering makes this inherent);
  empty-items InvalidArgument delegation to archive writer not directly exercised by a test.
Task 5: complete (commits 90608b5..8d99c49, review clean - Approved, opus; 0 Critical/Important)
  PROCESS: implementer cancelled by operator mid-verification (full -L atx_vol gate killed on purpose;
  operator runs full suite later). Controller ran targeted gate itself (SurfaceDb|SurfaceArchive 33/33 PASS),
  committed, and wrote task-5-report.md. Reviewer verified the apply_fit_preset mirror faithfulness risk
  clean (all carried fields captured; uncarried fields re-supplied via apply_fit_preset-first order).
  FULL-MODULE GATE (-L atx_vol) DEFERRED TO OPERATOR — not evidence-backed in this worktree.
  Minor (roll-up): PinnedConfig_OverridesPreset three flag assertions non-discriminating vs Hft defaults
  (correction_cache/score_parity/calendar_floor already false); market-snapshot preservation asserted only
  for S/r/now_ts_ns (expiry_rates/cash_divs by inspection); apply_symbol_config could be noexcept to match
  apply_fit_preset.

FINAL WHOLE-BRANCH REVIEW (fable, 896a100..d9d852e): "With fixes" -> fix wave 3f12b43 -> re-verified
  "Ready to merge: Yes". Findings fixed: (Imp#1) writer-side enum wire-range validation in
  write_db_manifest + persist_locked reordered to parse-validate BEFORE rename (bricking hazard closed;
  new test SurfaceDb.UpsertBadEnum_FailsCleanly_DbStillOpens); (Imp#2) drop_partition unlink moved inside
  lock + header thread-safety paragraph corrected (same-key partition mutations must not race in-process;
  write_partition pre-lock archive write is plan-mandated residual window, documented); (Min#4) Task-N
  scaffolding comment sweep across all three product files. Targeted suite 34/34 PASS, /WX clean.
  Roll-up triage: all remaining Minors accepted as post-merge follow-ups (refresh magic peek,
  find_partition canonicalize_key unification, enum-cap static_asserts, SymPlan/PartPlan scaffolding,
  find_slot std::string alloc on >15-char symbols, Windows rename-while-open transient note).
  OUTSTANDING BEFORE MERGE: full-module gate `& .\scripts\atx-build.ps1 -Ctest -L atx_vol` on 3f12b43 —
  DEFERRED TO OPERATOR by explicit instruction (targeted SurfaceDb|SurfaceArchive 34/34 is the evidence
  on record). Feature branch worktree-feat-atx-vol-surface-db is otherwise merge-ready.

============================================================
# SDD progress — MAG7 vs SPY dispersion-strangle backtest (2026-07-11)

Goal doc: atx-vol/sprints/2026-07-11-atx-vol-mag7-dispersion-strangle-backtest-goal.md
Plan: docs/superpowers/plans/2026-07-11-atx-vol-mag7-dispersion-backtest.md
Worktree: C:/atx/.claude/worktrees/feat-atx-vol-mag7-dispersion (branch worktree-feat-atx-vol-mag7-dispersion, rebased onto main @ 750a286)

Process notes:
- Full `-L atx_vol` gate DEFERRED TO OPERATOR (standing instruction); targeted suites only.
- `.superpowers/sdd/progress.md` is TRACKED on main-line history; this feature APPENDS its
  section (append-only keeps the eventual merge clean). Ledger commits: `git add -f`, `chore(sdd): ...`.
- Task 8 (real-data pull/run) is controller-led + operator-gated (paid Databento pulls, ~$150 cap).
- Goal-doc stale point (recon-verified): bulk pull tool `databento_bulk_opra` already exists in
  atx-core (produced the 123-day SPY hive data/spy_ytd/opra/SPY); Task 8 reuses it.
- Baseline (post-rebase pending re-verify): pre-rebase worktree ran 81/81 targeted green at fb6e7c1.

## Tasks (MAG7 dispersion)

- Task 1: Clock::from_surface_db (SurfaceDb-backed clock) — COMPLETE
- Task 2: CloseAtHorizon lifecycle + missing-name policy — COMPLETE
- Task 3: make_dispersion_strangle_spec builder — COMPLETE
- Task 4: run_report emitters — COMPLETE
- Task 5: populate_surface_db + mag7_surfdb_populate example — COMPLETE
- Task 6: mag7_dispersion_backtest example + gate test — COMPLETE
- Task 7: tools/mag7_dispersion_report.py + python test — COMPLETE
- Task 8: real-data pull/populate/run/report (operator-gated) — PENDING
- Final whole-branch review — PENDING

## Minor findings roll-up (MAG7 dispersion, for final review triage)
(none yet)

## Log (MAG7 dispersion)

Baseline post-rebase onto 750a286: 99/99 targeted (Strategy|Backtest|Dispersion|SurfaceDb|SurfaceArchive|TearSheet) green at 6198a27.
Reconfigured build with -DATX_BUILD_EXAMPLES=ON (needed for example targets in T5/T6/T8).

Task 1: complete (commits 6198a27..5b53d1c, review clean — SPEC ✅ / Approved; 0 Critical/Important)
  58/58 targeted (SurfaceDbBacktest|SurfaceDb|SurfaceArchive|Backtest) green; TDD RED→GREEN evidenced.
  Deviation (accepted): brief snippet `p.key + kSurfaceDbPartitionExt` doesn't compile (string+string_view);
  implementer used `p.key + std::string(kSurfaceDbPartitionExt)` — behaviorally identical.
  Minor (roll-up): backtest.hpp:40-41 Clock class comment still describes only the corpus-manifest route;
  mention from_surface_db.
  NOTE: .superpowers/sdd/task-N-brief/report.md files are TRACKED with stale main-line content —
  implementers overwrite per task; expect rewrite diffs, harmless.

Task 2: complete (commits be6a7f5..f9f14df, review clean — SPEC ✅ / Approved; 0 Critical/Important)
  67/67 targeted (Strategy|Backtest|Dispersion) green; resolve_spec refactored to shared resolve_spec_impl
  (verified no duplication); no-trade contract mirrors DispersionStrategy; close pass before entry.
  Interface note for later tasks: resolve_spec_with_policy min_names counts LegSpec (name) granularity.
  Minor (roll-up): (a) no test for close-pass firing on a day whose entry no-trades with a non-empty book
  (ordering correct by inspection; T6 gate test partially covers); (b) CloseAtHorizon+EveryNDays cadence
  combination untested (shared code path with HoldToExpiry); (c) ResolveDrop.symbol empty for uid-only
  legs (diagnostic-quality gap).

Task 3: complete (commits 766f7bf..922a6ad, review clean — SPEC ✅ / Approved; 0 Critical/Important)
  47/47 targeted (DispersionStrangle|Strategy|Dispersion) green; all 9 validation rules implemented+tested;
  acceptance math verified through real resolve_spec_with_policy (40Δ reprice, equal theta, net vega flat).
  Minor (roll-up): (a) no duplicate-name guard in cfg.names (dup name silently double-sizes theta);
  (b) spec.name hardcoded "mag7_dispersion_strangle" regardless of basket contents (cosmetic).

Task 4: complete (commits f48c3d9..a70bf10, review clean — SPEC ✅ / Approved; 0 Critical/Important)
  11/11 targeted (RunReport|TearSheet) green; pinned 27-column series header + all metric key sets verified
  byte-for-byte; single shared write_meta_body; all divide-by-zero guards traced.
  Minor (roll-up): (a) no CSV escaping (matches tearsheet.cpp convention; note for Python consumer);
  (b) db-stats appended meta can duplicate a colliding caller key (spec-literal, self-flagged);
  (c) fmt10/fmt_i64/fmt_u64 could be one template (style); (d) redundant defensive sort of partitions().

Task 5: complete (commits 73c6996..05bc3eb + fix 1347d7d, review "Approved" opus, 1 Important fixed →
  re-verified "Spec ✅ / Approved"; 0 remaining Critical/Important)
  8/8 SurfaceDbPopulate + 57/57 db/corpus + 67/67 fit-path regression sweep green.
  ACCEPTED DEVIATION (reviewer-judged justified-extra): fit_board extracted verbatim from corpus.cpp into
  private src/corpus_board_fit.{hpp,cpp} (pure move, verified vs pre-image); NEW additive `session_overlay`
  hook on PricerFitter::fit (default-empty, verified inert at ALL existing call sites) — needed because
  apply_symbol_config sets SessionInputs fields PricerConfig can't carry (band_k, al_opts, calendar_repair,
  pinned calib). Fix wave added SurfaceDbPopulate.SymbolConfigOverlayReachesFit (al_override oracle,
  neuter-RED/restore-GREEN evidenced).
  Minor (roll-up): (a) skip_existing uses open_partition (opens+CRC-validates; corrupt existing partition
  aborts populate instead of skipping — membership check via partitions() would be resume-robust);
  (b) example upserts symbol_config_from_preset(parsed --preset) vs constraint's literal FitPreset::Fast
  (default matches; flag-honoring defensible).

Task 6: complete (commits 3a415fc..b5c05f9, review clean — SPEC ✅ / Approved; 0 Critical/Important)
  11/11 targeted green; example 306 lines; 18-key shared meta verified byte-for-byte across all emit calls;
  live binary smoke (4 files + exit codes 2/2/1) reported by implementer.
  Minor (roll-up): (a) expect_result_bit_identical trimmed to 7 columns vs spy pattern (drops
  pnl_theta/gamma/vega; restore cheap); (b) example's literal 18-key MetaKv not regression-tested (gate
  test uses its own 2-key meta); (c) populate_stats.csv copy branch untested (manual smoke only);
  (d) spec-validation CLI errors exit 1 not 2 (judgment call, flagged); (e) console peak_lots recomputed
  vs reusing result_summary_metrics.

Task 7: complete (commits b592be9..a88cb05 + fix 55b9e5f, review Approved, 1 Important fixed →
  re-verified "Spec ✅ / Approved"; 0 remaining Critical/Important)
  8/8 python tests green; ctest registration verified by controller post-reconfigure
  (Mag7DispersionReport Passed, TearSheet suite 6/6). Reviewer verified fixture format fidelity
  byte-for-byte vs real emitters, SVG sanitization empirically (xmlns/RDF strip safe for HTML5 inline),
  id-collision fix real (294/294 unique ids). Fix wave: _fmt_value ±inf guard + 5 formatter unit tests.
  Minor (roll-up): (a) exact-1.0 fractions render bare "1" in tables; (b) fixture max_drawdown sign
  cosmetics; (c) matplotlib per-chart <style> universal selector scoping note (no current consequence).
