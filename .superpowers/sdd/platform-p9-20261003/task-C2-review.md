# Lane C2 review (P9 wave 2: `nav --calibrate-gross`, `gross_match: pm6-6-engine`, M1c-RED)

## Verdict

**CHANGES_REQUIRED** (3 Required, 7 Suggested). Spec compliance: PASS except R3 (the "numpy's mean" contract).
M1c-RED fix: PASS (sound; no objection). C++ calibration and construction reuse: PASS. Engine match stage
(Python): CHANGES_REQUIRED (R1, R2).

## Reviewed SHA

`08297183` on `feat/p9-c2-20261003` (pool-15), base `ad406718`. Lane commits `8f91f897` (M1c-RED), `7eacc173`
(calibrate-gross), `7c514b41` (engine match), `7358b032` (C1 minors), `08297183` (report).

## Evidence (run by the reviewer)

- `PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
  scripts/tests/test_research_wave.py` (cwd pool-15): `17 passed in 28.48s`, exit 0.
- Same flags, cwd `scripts/tests`: `test_no_python_mirror.py test_wave_driver.py test_wave_hardening.py
  test_wave_queue.py test_wave_review.py test_wave_review_minor.py test_wave_scoreboard.py test_wave_speed.py`:
  `G-P5: 37 mirrored rule function(s) in 14 row(s) left for P9 lanes` / `77 passed in 196.25s`, exit 0.
- numpy check (scratch script, deleted): the five pinned bit patterns of `CalibrateGross.NumpyMeanIsNumpysPairwiseMean`
  equal numpy 1.26.4 (installed) and the lane's algorithm at n = 5, 100, 129, 1000, 1023. Lane algorithm vs
  `np.ndarray.mean` on 360 random series: **23 mismatches, all at n in {8193, 9000, 20001}**; the model
  `acc = 0.0; acc += pairwise(block) per 8192-value block; acc / n` matches 360/360 (R3).
- ref-leak check (scratch script, deleted): `calibrated_doc(lib-v81.json)` then research_cycle's ref merge
  (`research_cycle.py:1210`) gives a ref argv ending `--calibrate-gross 0.9862 --calibrate-tolerance 0.005` (R1).
- Lane receipts present: `build-equity/mega-p9-c2-f-receipt.json` (equity-rel, `7c514b41`, 0 dirty, exit 0, the only
  log warnings are clang-cl's `/MP unused`), `-g` (equity-dev, dirty over `7c514b41` = the `7358b032` content, exit
  0). No rebuild by the reviewer: no doubt needed a run. Lane object trees are gone (DISK-2).

## M1c-RED (`8f91f897`, `stats_ext.hpp`) -- sound

- Same function: coefficients untouched (`kAcklamA..D`), branch boundaries untouched (`p < kAcklamPLow`,
  `p <= kAcklamPHigh`), Horner order identical (C: 6 coefs; D: 4 coefs then `* q + 1.0`; central A(r) `* q /`
  (B(r) `* r + 1.0`); upper `-C / D`, and `(-X)/Y` is bitwise `-(X/Y)`). The rest of norm_ppf has no multiply
  feeding an add inside one expression (`norm_cdf(x) - p` crosses a call; `x * u / 2.0` feeds a divide), so under
  `-ffp-contract=on` nothing else is contractible.
- "NAV identity cannot move" holds on equity-dev / equity-rel: on a non-FMA target a run-time `llvm.fmuladd` lowers
  to mulsd + addsd, the exact sequence `mul_add_unfused` writes; only compile-time folds change (now unfused = run
  time). The 20M-input harness (every norm-score-v1 `u = k / (2 (n+1))`, n <= 4000, which is exactly the kernel's
  `(b + e + 1) / denominator` quotient, plus 4M tail/central p) shows 0 mismatches under both flag sets and its AVX2
  control proves sensitivity. The object diff (one function differs: out-of-line norm_ppf, SLP mulpd/addpd vs scalar,
  same per-lane IEEE ops; no FMA in either) is convincing.
- Other callers: normal_score.hpp:61, deflated_sharpe.hpp:123/124/166/167, haircut.hpp:66, min_trl.hpp:38,
  decay_monitor.hpp:213, strategy_mine_rule.cpp:26, python bind_eval: all run-time arguments on the research paths.
  The only production constant-argument call is `validation/bias_audit.cpp:31` (DSR at constant N, a boolean against
  0.5 thresholds): no pinned value can move. `cluster_eval.hpp:577` passes N = 1 (no norm_ppf). Header otherwise only
  gains two `detail::` helpers (no name collisions in the tree).
- No expected value edited (`book_normal_score_test.cpp`: comment + appended test only).
- `NormPpfOfALiteralIsItsRunTimeValue` has teeth in Release (old header: 10/12 and 1001/1002 fold fused, differ by
  1 ULP from the volatile-read call; handoff + `c2-ppf-fma.py`), none in Debug (test TU `/Od` folds nothing) -- that
  is inherent and fine.

## Spec compliance (brief + carry + rulings, item by item)

| item | status |
|---|---|
| Brief: pass 1 at L, S2 only, cached desired targets, no capacity books | PASS (`calibration_pass`, `ConstructionTape`) |
| Brief: one correction `L x G_P / G`, 4 decimals, pass 2 same process, one load | PASS (`(L*G_P)/G` order = `wave_rules.matched_leverage`; `to_chars(fixed,4)` = `f"{x:.4f}"`, tie 1.03125 pinned) |
| Brief: refusal when L' leaves [1, 2] | PASS (before the output dir; range duplicates `strategy_target_replay.cpp:131`, S4) |
| Brief: G "as numpy" | **FAIL for n > 8192** (R3); exact for n <= 8192 |
| Brief gtests WithinToleranceOnePass / OneCorrectionMatchesTwoProcess / RefusesOutsideRange | PASS (+4 more) |
| Brief: `wave_rules.py:110-119` deleted after root identity | PASS (correctly not deleted, G-P5) |
| C2-1 record in summary.json, not recipe.json; capacity summary untouched | PASS (`publish_nav`, `!capacity_pass`) |
| C2-2 run stage stops after w only behind the manifest key | PASS (`engine(w)`; absent / pm6-6 / none = unchanged path and dicts) |
| C2-3 matched false publishes, match stage stops | PASS (exe publishes; `match_engine` raises) |
| C2-4 match stage pins spec digest + w digest in engine mode | **MISSING** -> root merge item (not a lane defect): `match_inputs` (`wave_stage_cell.py:352-354`) returns `{}` when `run.nav` is None |
| C2-5 / C2-RED | PASS (above) |
| P15 manifest key, absent = Python stage | PASS |
| Carried: null store + risk-target ranges | PASS; cannot fire on a pinned spec: the v7 CLI already refuses both (`strategy_nav_v7.cpp:1300-1305`), `BookScaler` refused a null store at its first decision, the engine re-validates (`risk_target.cpp:164`) |
| Carried: K-P9-7 `required` | PASS; strictly more permissive (elements removed; min/max/default kept). E2's PSCHEMA reader handles `required: []` (`pool-17 wave_kinds.py:285`); its tests use their own row tables |
| Carried: C1-SPO summary pin; adv-hold+capacity 5 passes | PASS (pinned via `v7::extend_summary`; the 5 passes justified) |
| Flag-absent identity (C++) | PASS by reading: 7-arg overload forwards `NavCalibrateOptions{}`; `requested` = old `base`; `published` = cfg; tape null; `replay_books(...,nullptr,true)` = `replay_observed`; no summary key. `nav --list-rules` output gains a capability and smaller `required` arrays (no pinned consumer found) |
| No trial run, no ledger append, no data opened | PASS |

## Construction reuse ("where valid")

Read against `run_books` / `decide_construction` / `plan_decision` / `close_book`: books read only the effective
rebalance, census, construction record, `shared.desired`, `tiers.tier/missing` (all taped), `shared.state.fast_before`
(two-speed: excluded) and, for the export, `shared.hold_in` (excluded with a declared band). Invalid cases and their
exclusion: adv-hold-v1 (cap reads L and the multiple's NAV through `ctxs.front()`) -> off; two-speed-v1 (F at L) ->
off; `--emit-holdings` + declared hold band -> off; several lockstep groups -> unreachable (only with adv-hold) and
refused anyway; cadence/calendar mismatch -> Internal. inv-vol, neutralize ring, locate-in-aim mask, hold-band state,
tier scan: construction-internal, read by no book. Pass-1-S2-alone = S2-in-lockstep holds (window/cache inputs are the
base config's). No uncovered invalid case found. Test gap: S2.

## Findings

`path:line | severity | problem | required fix`

- **R1** `scripts/wave_stage_cell.py:259-276` | **Required** (major) | `calibrated_doc` adds `--calibrate-gross
  G_parent --calibrate-tolerance T` to `nav.flags`, but the `ref` phase inherits `nav.flags` (`research_cycle.py:1210`;
  none of the 17 specs with a `ref` sets `ref.flags`). The committed, digest-pinned `-gm` spec therefore describes a
  CALIBRATED reference construction (pass 1 vs G_parent, possibly a corrected L, a `calibration` key in its
  summary). The wave masks it only because the run stage (`--stop-after w`, ref precedes w) already wrote
  `ref.output` and the -gm cycle reuses it by summary.json presence; a fresh run of the spec of record runs the
  wrong reference and can fail its own ref-compare. Python mode avoids this exactly (`wave_steps.py:162-163` pins
  `ref.leverage`). | For a non-template doc, pin the ref to the pre-calibration nav: `ref.flags` = the original
  `nav.flags` (and `ref.leverage` = L when absent), as `gm_doc` does; a pytest asserting the -gm ref argv carries no
  `--calibrate-*`.
- **R2** `scripts/wave_stage_cell.py:323` (-> `wave_result.py:41,75`, `research_spec.py:176`) | **Required** (major;
  fix or PM ruling) | Engine mode makes the calibrate RECIPE the cell of record: `cell_spec` = the -gm whose
  `nav.leverage` is the requested L (not the published L') and whose `nav.flags` carry `--calibrate-gross <G_parent>`.
  `next_parent.spec` = that file, and `wave_queue.py emit --after` / rule-cell templates resolve children by deep copy
  of the parent, so the next wave's cells (a) start at the grandparent's L instead of the parent's matched L'
  (PM6-6's "calibration run at the parent's L" broken), and (b) inherit a stale `--calibrate-gross`: a following
  Python-mode wave's calibration run would itself be calibrated toward the old target and the Python match would
  read that NAV's G as G(L) -- silent miscalibration. Python mode's -gm is a plain spec at L'. | Publish a plain
  cell-of-record at L' (e.g. write the recipe under its own name and the -gm as `gm_doc` at L' bound to the engine NAV
  dir, or strip `--calibrate-*` and set `nav.leverage` = L' wherever a spec is derived from a parent), or get a PM
  ruling that engine-mode cells may not be parents; plus a pytest that a parent from an engine wave yields children
  without `--calibrate-*` at L'.
- **R3** `atx-impl/src/strategy_nav_calibrate.cpp:65-70` (doc `.hpp:45-49`) | **Required** (minor-latent; the stated
  contract is false) | `numpy_mean` = `0.0 + pairwise(all) / n` is numpy's mean only for n <= 8192: numpy's buffered
  reduction sums 8192-value blocks pairwise and accumulates the blocks in order (verified 0/360 vs 23/360). Latent for
  the current <= ~3k-row NAVs, and the engine stage's `g2 == cal.gross` check fails closed, but pass 1's G (never
  cross-checked) drives the correction, and ledger candidate 2 would record a false fact. | Accumulate per 8192-value
  block (`acc = 0.0; for each block: acc += pairwise_sum(block)`); add numpy-pinned cases at n = 8193 and > 16384;
  fix the comments and ledger candidate 2.
- S1 `atx-impl/src/strategy_nav_calibrate.cpp:74`, `atx-impl/src/strategy_nav_replay.cpp:1314` | Suggested | 101
  columns (100 limit); the two CMake comment tails are 103 / 110. | Wrap.
- S2 `atx-impl/tests/strategy_nav_calibrate_test.cpp` | Suggested | No test asserts `construction_reused: false` for
  adv-hold, two-speed, holdings + declared band or the byte budget, and no reuse test runs X-5's flag set
  (warm start, neutralize, locate-in-aim, delta basis, liquidity cache, capacity); root's real-data gm pair is the
  only cover. | Add one fixture gtest per exclusion and one X-5-shaped reuse identity.
- S3 `atx-engine/include/atx/engine/eval/stats_ext.hpp:84-86,126-129` | Suggested | "same two-rounding value, on
  every build and every target ISA" over-claims: folds of `log/exp/erfc` use the compiler's math, so literal = run
  time still rests on those agreeing (the new test pins 7 points; green in Release). | Say "the polynomials" and
  name the residual dependence.
- S4 `atx-impl/src/strategy_nav_calibrate.hpp:33` | Suggested | [1, 2] duplicates `validate_target_config`
  (`strategy_target_replay.cpp:131`) and `leverage_cap()`. | One shared constant.
- S5 `task-C2-report.md:192-193` | Suggested | Root step 3 says the engine `cell` keys "equal the Python mode's"; by
  design `spec`, `nav`, `spec_digest`, `commit`, `readers` differ (engine is always -gm). The C++-vs-Python equality is
  emulated in the pytest (`FakeCycle.calibrate` uses `WR.matched_leverage`) and OneCorrectionMatchesTwoProcess derives
  L' with the implementation's own functions (pinned separately) -- root's gm pair is the real cross-check. | Reword:
  compare leverage, gross, g_parent, g_calibration, leverage_calibration, corrected and the NAV bytes.
- S6 `atx-impl/src/strategy_nav_replay.cpp:3481-3496` | Suggested | With cadence 1 the tape holds sessions x names x
  (8 + 2) B; X-5's `--max-bytes 1073741824` may leave no room, so reuse may silently be off (recorded in
  `construction_reused`) and the wall gain smaller than claimed. | Root reads `construction_reused` in the gm pair.
- S7 `atx-impl/tests/strategy_vol_target_test.cpp` | Suggested | `nav --list-rules` capability
  `calibrate-gross-pm6-6` is unpinned. | One EXPECT on the capabilities array.

## What root must check at merge

1. Build equity-dev and equity-rel: `atx-impl-strategy-target-tests`, `atx-equity-strategy-targets`,
   `atx-engine-eval-tests`, `atx-engine-book-tests`, `atx-impl-tests` (glob compiles the new test; never built by the
   lane); gtest filters as in the report; `BookNormalScore.*` green in Release (M1c-RED lifted).
2. Identity runs 1-3 of the report (flag-absent X-5 NAV; X-5 gm pair; engine wave) -- step 3 per S5.
3. C2-4: add to `match_inputs` in engine mode the spec stage's `spec_digest` and the w output digest.
4. Keep-both merges: E2 also edits `scripts/wave_stage_cell.py` (5 lines) and `wave_manifest.py`; COV's tail in
   `atx-impl/tests/CMakeLists.txt` (`atx-impl-strategy-target-tests`).
5. Ledger: candidate 1 is correct; candidate 2 is false for n > 8192 (R3) -- amend before appending.
6. The `rel` preset (IPO ON) can fold more constant-argument norm_ppf calls; like rel-avx2 (C2-5) its values move
   to the run-time ones. Confirm no reference was produced under `rel`.
7. Delete `wave_rules.py:110-119` only after the identity run (G-P5).

## Checked

- [x] `.agents/cpp/agent.md` §10 on the C++ diff (bounded recursion annotated; error paths fail before publish;
  no UB, narrowing or lifetime issue found; /W4 /WX built clean per receipts; 100 columns: S1)
- [x] Diff inside scope + listed cross-lane edits (stats_ext.hpp, book test, tests CMake, spo_v3 test, wave_rules,
  wave_stage_cell, wave_fixture, test_research_wave); no COV-owned file touched
- [x] Report evidence matches claims where re-runnable (pytest both groups; numpy pins; receipts)
