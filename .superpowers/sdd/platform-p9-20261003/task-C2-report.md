# Task C2 report: `nav --calibrate-gross` in C++ and the M1c-RED fix (P9 wave 2, slot 6)

## Outcome

**DONE_WITH_CONCERNS** (minor; see "Concerns / open"). M1c-RED fixed with its root cause; both Release reds green
in Debug and Release, no expected value edited, run-time NAV values shown unchanged. `nav --calibrate-gross` in C++
with the cached construction, the engine match stage behind `gross_match: pm6-6-engine`, the brief's three gtests plus
four more, and three of the four carried C1 minors.

- Pool `C:/atx-wt/pool-15`, branch `feat/p9-c2-20261003`, base `ad406718` (handoff-C2.md), resumed from the
  hand-off commit `0c27cf79`. Commits on the lane: `8f91f897` M1c-RED fix; `7eacc173` `--calibrate-gross` (C++, gtests, the
  store / range minor); `7c514b41` the engine match stage (Python, pytest); `7358b032` the K-P9-7 and C1-SPO minors;
  then this report.
- Rulings applied: C2-1 (calibration record in summary.json, not recipe.json), C2-2 (engine mode stops the run
  stage before the NAV, behind the manifest key; key absent = the Python match, byte-identical), C2-3 (pass 2 outside
  the tolerance publishes `matched: false`, the match stage stops), C2-RED (contraction-free polynomial in
  `stats_ext.hpp`, no expected value edited, both Release reds green in Debug and Release), W2-BUILD (every build
  through `scripts/research-build.ps1`, own pool, one at a time, gate checked first), DISK / DISK-2 (below).

## M1c-RED (required)

Root cause (handoff-C2.md, reproduced there bit for bit): `eval::norm_ppf`'s four Horner polynomials were single
expressions `acc * x + c`, which clang-cl's default `/fp:precise` (`-ffp-contract=on`) emits as `llvm.fmuladd`. On the
x86-64 baseline (no FMA; equity-dev / equity-rel have no `/arch:AVX2`) a RUN-TIME fmuladd lowers to `mulsd` + `addsd`
(two roundings), but Release constant-folds a call whose argument is a literal (`norm_ppf(10.0 / 12.0)` in the test)
and folds fmuladd FUSED (one rounding). The Acklam start then differs in its last bit and the Halley step keeps it:
`10/12` gives ...082 (kernel, run time) vs ...071 (folded literal); `1001/1002` ...887 vs ...891. Debug passed because
the test TU is `/Od` (nothing folded).

Fix (`8f91f897`): `detail::mul_add_unfused(acc, x, c)` (the product is its own full expression, so contraction cannot
fuse it; no `contract` flag is set under "on", so the backend does not either) and `detail::horner_unfused` (the same
rounding sequence as the nested expression with an unfused multiply and add); `norm_ppf` evaluates all four
polynomials through them. The rest of `norm_ppf` has no multiply feeding an add. Expected values untouched; added
`BookNormalScore.NormPpfOfALiteralIsItsRunTimeValue` (literal vs a `volatile`-read argument at 10/12, 5/12,
1001/1002, Blom, rank-over-n, a lower-tail p and 1/1002).

Results: Debug `atx-impl-strategy-target-tests` `BookNormalScore.*:NormScore.*` 10/10 (11/11 with the new test), whole
target tests 339/339 (p9-c2-a) and 346/346 after the calibrate commit (p9-c2-e). Release (equity-rel, p9-c2-f):
built at `7c514b41`, clean tree, 1502 s, 306 TUs, 0 warnings; `BookNormalScore.*:NormScore.*` 10/10 with the two
former reds `TiesShareTheMeanRankAndMirrorsAreOpposite` and `FixtureTellsWrongRulesApart` green, and the whole Release
target-tests binary 347/347 (CalibrateGross.* included). The final Debug binary (p9-c2-g) is 347/347 as well.

### How the existing NAV identity was checked (it does not move)

Evidence files (this dir): `c2-identity-evidence.txt`, `c2-ppf-identity.cpp`, `c2-mk-identity.py`, `c2-fndiff.py`.

1. Run-time values, old vs new, bit for bit: `c2-ppf-identity.cpp` compiles the committed `norm_ppf` and the pre-fix
   one (`git show 8f91f897~1:atx-engine/include/atx/engine/eval/stats_ext.hpp`, wrapped in a namespace) into one
   program and evaluates both at run time on EVERY u the norm-score-v1 kernel can form for n <= 4000 names
   (k / (2 (n + 1)), k in [2, 2n]) plus 4M pseudo-random p including both tails: 19,999,999 evaluations, **0
   mismatches** under the Release flags (/O2 /DNDEBUG -MD) and **0** under the Debug flags (/Od /RTC1 -MDd). Control
   (the harness can see a difference): the same program under `/arch:AVX2` (FMA at run time) has 7,864,564 mismatches.
2. Machine code of the NAV path's only norm_ppf caller: `atx-impl/src/strategy_target_replay.cpp` (normal_scores of
   norm-score-v1; every other TU that reaches norm_ppf is outside the nav exe: IC / mine / engine eval) compiled with
   each pool-15 tree's own flags (compile_commands.json; PCH dropped so both variants parse alike) against the new and
   the old header: of 2087 (Release) and 2691 (Debug, an /O2 TU there) functions exactly ONE differs, the out-of-line
   `norm_ppf` itself (the old one SLP-vectorised the numerator and denominator polynomials into mulpd/addpd lanes; the
   new one is scalar mulsd/addsd: the same per-lane IEEE roundings), and neither object holds a fused multiply-add.
   `/showIncludes` confirms which header each variant read.
3. The gtests that pin NAV bytes on synthetic fixtures pass unchanged in Debug (whole target tests above).

So on equity-dev and equity-rel every run-time norm_ppf value, hence every norm-score-v1 book and every other NAV
byte, is unchanged. What moves: (a) compile-time folds of a literal argument (tests only; they now equal run time),
(b) a `rel-avx2` build (FMA at run time): its norm_ppf now equals the baseline's instead of the fused value. Root's
confirmation runs are listed under "How root verifies".

## `nav --calibrate-gross` (C++, `7eacc173`)

`nav ... --aim-leverage L --calibrate-gross G_P [--calibrate-tolerance .005]` (`strategy_nav_calibrate.{hpp,cpp}`,
`run_nav_replay` 8-argument overload, `dispatch_nav_replay`; the v7 seam forwards the tokens unchanged):

1. One admission and load (`admit_and_load` at L, all books), shared by both passes: fields, liquidity, signal.
2. Pass 1: the primary S2 book alone (`scenarios[nav_primary_scenario_index]`, no other main book, no capacity book, no
   observer, `v7::observe` off so the seam's records stay pass 2's). G = `numpy_mean` of its `days[].gross_leverage`
   over every row: `0.0 + pairwise_sum / n` exactly as numpy 1.26 (8 accumulators to 128 values, split at n/2 rounded
   down to a multiple of 8); checked against numpy on 3000 random series (0 mismatches; the first-element-initial
   variant mismatched 1218 times) and pinned in `CalibrateGross.NumpyMeanIsNumpysPairwiseMean`.
3. PM6-6: |G - G_P| <= T keeps L; else L' = L x G_P / G written `std::to_chars(fixed, 4)` (Ryu, ties to even, as
   Python's `f"{x:.4f}"`; 1.03125 -> 1.0312 pinned) and parsed back; L' outside [1, 2] is InvalidArgument before the
   output directory exists; G not finite and > 0 likewise.
4. Pass 2: the plain run at the final L (`published` = cfg with `aim_leverage` L'), every file its bytes; summary.json
   gains `calibration {target, tolerance, leverage_requested, gross_requested, corrected, leverage, gross, matched,
   construction_reused, rule}` (main directory only; not the capacity summary; recipe.json untouched, so the decide
   path's recipe pin of the plain argv at L' holds). `matched = |G' - G_P| <= T` of pass 2's S2 book; false still
   publishes (C2-3); the console prints both passes.
5. Cached construction (the brief's "cached desired targets"): pass 1 records, per decision, the effective rebalance,
   tier census, construction record, the desired target after every cadence decision and (with fields) the tier and
   missing flags; pass 2 restores them instead of `decide_construction` (Internal on any calendar mismatch or an
   unconsumed record). Off, pass 2 forming it again (same bytes), when the construction reads L or state pass 1 does not
   form: `--adv-hold-q` (cap reads L and the multiple's NAV), `--two-speed` (F moves toward L; DECIDE reads it), or
   `--emit-holdings` with a declared hold band (the export reads the band state); and when the record (decisions x
   (sizeof record + names x (8 + 2 tiered) B)) does not fit what `--max-bytes` leaves after the reserve and the load
   (`max_working_bytes` is a recipe key, so it is never raised). `construction_reused` records which.
6. Refusals: `--calibrate-tolerance` without the gross, a non-finite gross (usage, exit 2); G_P or T not > 0, a rule
   other than aim-partial-v5, an spo rule (`v7::shared_plan_state`: its engines keep pass 1's rows) (exit 1);
   `--construction-grid` (exit 2). `nav --list-rules` capabilities gain `calibrate-gross-pm6-6`.

Cost: pass 1 = the construction + one book; pass 2 = the books without the construction (when reused). A plain run is
the construction + all books; the two-process flow was two loads + two full runs when it corrected.

## Match stage integration (Python, `7c514b41`; P15, cross-lane)

`scripts/wave_rules.py`: `GROSS_MATCH["pm6-6-engine"] = {tolerance .005, corrections 1, engine True}` (docstring
line added). `wave_manifest.validate` already checks the name against GROSS_MATCH (no `wave_manifest.py` edit; its
doc line `"gross_match": "pm6-6" | "none"` is E2's to extend). Absent / `pm6-6` / `none`: today's code path, the same
output dicts.

`scripts/wave_stage_cell.py` under the engine key: `run_stage` runs the cell `--stop-after w` (every cell has a w phase:
`Wave.phase_bases` reads `ic.w_output` unconditionally) and returns `{spec, nav: None, engine: True, phases}`;
`match_engine` reads G_parent with the mechanics reader (`mech-parent`), writes `<cell>-gm.json` at the cell's own L
with nav output `<output>-gm` and `nav.flags + --calibrate-gross repr(G_parent) --calibrate-tolerance repr(T)`
(template specs: `change.set nav.output` + `change.flags.nav`), locks, commits, runs ONE `--stop-after nav`, reads only
summary.json's `calibration` block, refuses (EXIT_PIN) a missing record or a calibration at another L / target, checks
the mechanics reader's G of the published NAV equals the exe's `gross` bit for bit (a live C++-vs-numpy cross-check on
every engine wave), stops on `matched: false`, and returns the Python match's keys (`corrected`, `leverage` as the 4-dp
string when corrected, `g_parent`, `g_calibration`, `leverage_calibration`, `g_cell`, `nav`, `cell_spec`,
`spec_digest`, `phases`, `readers`, `commit`) plus `engine` and `calibration`. `run_plan` / `match_plan` print the
engine commands. `wave_rules.py:110-119` is NOT deleted: per G-P5 / plan section 0.6 that happens after root's
identity run, in a later slice.

## Carried C1 minors

| minor | outcome |
|---|---|
| config check skips a null risk store + risk-target parameter ranges | fixed in `validate_nav_config` (a non-fixed law without its store, risk-target-v1 S/b/C outside `validate_risk_target`'s ranges: InvalidArgument); gtest `NavBookRule.RefusedWithoutTheStoreOrOutsideTheParameterRanges` |
| K-P9-7 marks `aim_leverage` required while defaulting it | fixed in `risk_target::leverage_rules_json` (no rule lists aim_leverage as required; its default 1 stays); gtest `NavBookRule.AimLeverageIsOptionalWithItsDefault`. Contract note for E2 (D1-PSCHEMA reads `params_schema`): the `required` arrays shrank (fixed-v1's is now empty) |
| C1-SPO gtest pins recipe + holdings, not summary / extras | summary pinned: `SpoV3.CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes` now checks summary.json's `v7.declarations.capacity_spo_v3` (base, capped, base again). v7_extras.json's `capacity_spo_v3.rule` is the same function (`capacity_spo_v3_rule(s.adv_hold)`) but needs a full spo-v3 capacity run to pin: not added (cost vs a one-line shared call) |
| adv-hold + capacity runs 5 lockstep passes | not changed: by design each multiple's ADV cap reads m x NAV (P9 C1, Ruling E-15 superseded), so each multiple forms its own construction; sharing it would change bytes. A cost note only |

## Files changed

Owned (C1 NAV files + new): `atx-impl/src/strategy_nav_calibrate.{hpp,cpp}` (new), `atx-impl/src/strategy_nav_replay.
{hpp,cpp}`, `atx-impl/src/strategy_risk_target.cpp` (minor), `atx-impl/tests/strategy_nav_calibrate_test.cpp` (new),
`atx-impl/tests/strategy_nav_replay_test.cpp`, `atx-impl/tests/strategy_vol_target_test.cpp`,
`atx-impl/CMakeLists.txt` (tail: one `target_sources(atx-impl-core ...)`).

Cross-lane (listed): `atx-engine/include/atx/engine/eval/stats_ext.hpp` (Ruling C2-RED; norm_ppf is also used by
deflated_sharpe, haircut, min_trl, decay_monitor, strategy_mine_rule: run-time values unchanged, see above),
`atx-engine/tests/book/book_normal_score_test.cpp` (one test appended), `atx-impl/tests/CMakeLists.txt` (tail: one
`target_sources(atx-impl-strategy-target-tests ...)`; COV shares the list; `atx-impl-tests` picks the new file up by
its glob), `atx-impl/tests/strategy_spo_v3_test.cpp` (C1-SPO minor; COV owns the RiskStore section of
`strategy_spo.*` sources, not this test), `scripts/wave_rules.py`, `scripts/wave_stage_cell.py` (E2 / P15),
`scripts/tests/wave_fixture.py` (FakeCycle: `--stop-after w` writes no NAV; `nav --calibrate-gross` emulated with the
real `WR.matched_leverage`; `write_nav(calibration=...)`; `nav_gross`), `scripts/tests/test_research_wave.py` (two
tests). No `classes.json` literal (SQL2-CLS: none new).

## Tests

C++ (written and run here): `CalibrateGross.{NumpyMeanIsNumpysPairwiseMean, FourDecimalsAndTheStepArePm66,
WithinToleranceOnePass, OneCorrectionMatchesTwoProcess (plain and --capacity-curve, v7 files included),
RefusesOutsideRange}` (strategy_nav_calibrate_test.cpp), `CalibrateGross.TieredRunReusesTheConstructionByteForByte`
(strategy_nav_replay_test.cpp: mixed borrow tiers, --emit-holdings directory byte-identical too),
`NavBookRule.{RefusedWithoutTheStoreOrOutsideTheParameterRanges, AimLeverageIsOptionalWithItsDefault}`,
`BookNormalScore.NormPpfOfALiteralIsItsRunTimeValue`, `SpoV3.CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes`
(extended). The brief's three names are the three CLI tests; each compares the published directory with the
plain run's bytes (summary.json compared as JSON with `calibration` dropped).

Builds (all `scripts/research-build.ps1`, pool-15, one at a time after the gate check; receipts
`build-equity/mega-p9-c2-<t>-receipt.json`, 0 first-party warnings in every log): p9-c2-a (equity-dev, target tests,
912 s, the M1c fix), p9-c2-b (+ `atx-equity-strategy-targets`, 224 s), p9-c2-c / -d / -e (test iterations, 26-83 s),
p9-c2-f (equity-rel, target tests, 1502 s), p9-c2-g (equity-dev, target tests + targets exe at the final sources).
Debug `atx-impl-strategy-target-tests` whole: 339/339 (a), 346/346 (e), **347/347 (g, final)**; Release whole
**347/347 (f)**. Two test-only fixes during the loop, no production change: the CLI fixture's all-rows gross is about
.015, so an 8 % gap sat inside the absolute .005 and nothing corrected (the correction tests now pass a tighter
`--calibrate-tolerance`), and the tiered panel's volume was too thin for a $1bn book to build (scaled x 1e5).

pytest (explicit paths, Python 3.12): `scripts/tests/test_research_wave.py` 17 passed (two new:
`test_engine_gross_match_is_the_python_match_in_one_nav` -- the engine result's leverage, gross, G_parent, G_cal,
L_cal and corrected equal the two-NAV Python match's, one NAV instead of two; `test_engine_gross_match_within_tolerance
_keeps_the_leverage`); `test_no_python_mirror.py test_wave_driver.py test_wave_hardening.py test_wave_queue.py
test_wave_review.py test_wave_review_minor.py test_wave_scoreboard.py test_wave_speed.py` 77 passed.

## How root verifies

Builds (equity-dev and equity-rel): `atx-impl-strategy-target-tests`, `atx-equity-strategy-targets`,
`atx-engine-eval-tests`, `atx-engine-book-tests`, `atx-impl-tests` (the glob compiles the new test file; not built
here).

Anchored gtests, both builds:
- `atx-impl-strategy-target-tests --gtest_filter=BookNormalScore.*:NormScore.*:CalibrateGross.*:NavBookRule.*:VolTarget.*:SpoV3.CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes`, then the whole binary.
- `atx-engine-eval-tests --gtest_filter=*StatsExt*:*Deflated*:*MinTrl*` and `atx-engine-book-tests --gtest_filter=BookNormalScore.*:*DecayMonitor*` (handoff list; not run here).
- `atx-impl-tests --gtest_filter=CalibrateGross.*`.

Identity runs:
1. Flag absent (both changes): X-5 NAV with the new Debug exe vs `p9-c1-s2-x5-nav`: every file byte-identical except
   summary.json `/producer`. A norm-score-v1 book Release vs Debug now byte-identical except `/producer` (M1c-RED
   lifted: no `--rank-shape norm-score-v1` Release run was evidence until now).
2. X-5's gm pair: `nav <X-5 cell argv at L> --calibrate-gross <repr(G_parent)>` vs the two-process `-gm` NAV: every file
   byte-identical except summary.json `/calibration` (added) and `/producer` if the exe differs; `calibration.corrected`,
   `.leverage` (the -gm spec's L as a double) and `.gross` = the -gm NAV's mechanics G. Wall before (two runs + two
   loads) / after (one load, pass 1 S2-only, pass 2) from `--stage-timers` or the receipts.
3. The engine match stage on a wave whose manifest says `"gross_match": "pm6-6-engine"` (an X-5 replay of a corrected
   cell): wave_result `cell` keys equal the Python mode's. After that identity, the later slice deletes
   `wave_rules.py:110-119` (G-P5).

## Decisions

- Calibration record in summary.json (C2-1). The engine-mode cell spec is always the `-gm` copy (also when
  uncorrected), because its NAV is the one the stage ran.
- Pass 1 runs only S2 (brief); a consequence: under a warm start, an S2 book flat on score_begin refuses the run in
  pass 1 even if another main book built (the plain run checks all main books). Not seen in practice; a refusal, never
  wrong bytes.
- The tape covers exactly what books read of the shared construction; everything else (hold-band state, exposure
  ring, locate mask, tier scan) is construction-internal. Byte identity is pinned by WithinToleranceOnePass,
  OneCorrectionMatchesTwoProcess (capacity and v7 included) and the tiered test (tiers, census, holdings export).
- Tolerance in tests: the CLI fixture's all-rows gross is about .015, so the correction tests pass
  `--calibrate-tolerance 1e-4` (the tiered one 1e-6); the default .005 is pinned in WithinToleranceOnePass.
- The engine match reads summary.json's calibration block directly (leverage and gross: mechanics, not returns); every
  statistic still goes through the bounded readers.
- Evidence compiles for the identity were standalone clang-cl runs into the session scratch with each tree's
  compile_commands flags (no build tree or tracked file touched): the auto-mode classifier refused temporarily
  restoring the pre-fix header in the worktree, so the old header was compiled from a scratch copy instead.

## Concerns / open

- `match_inputs` pins the run stage's NAV summary; in engine mode there is none, so the match stage's resume pin is the
  spec stage's only (the -gm spec itself is committed and digest-recorded). Root may want a spec-digest pin there.
- `rel-avx2` norm_ppf values move (to the baseline's); any stored rel-avx2 norm-score output is superseded.
- The K-P9-7 `required` change reaches E2's PSCHEMA reader (more permissive).

## Disk (Rulings DISK, DISK-2)

After this report's commit: deleted the object trees this lane created in pool-15 -- `build-equity-rel/` (whole),
`build-equity/` except its `mega-p9-c2-*` receipts / build logs / cache-stats logs and the two directories that were
there before the lane (`audits/`, `v8-interim3-pitch-render-run2/`) -- and `deps/equity-rel/` (the per-worktree
FetchContent the Release configure fetched). Kept: every receipt and log named above, the evidence files in this dir.
The session scratch (`.../scratchpad/c2`, compiles, harness, logs) deleted by name. No child process left running.

## Ledger candidates

1. M1c-RED: clang-cl /fp:precise contracts `a*b + c` to llvm.fmuladd; Release constant-folds it FUSED while non-FMA run
   time is mul+add, so a literal-argument call and the run-time call differ by an ULP; write such polynomials as
   separate product and sum (stats_ext.hpp `mul_add_unfused`).
2. (amended in fix round 1, Ruling C2-R3; the round-0 wording held only for n <= 8192) numpy 1.26.4's contiguous
   float64 `mean` = (0.0, then += the pairwise sum of each 8192-value block in order (the ufunc buffer); pairwise = 8
   accumulators up to 128 values, split n/2 down to x8) / n; one pairwise sum over all n differs above 8192 rows (ramp
   at 8193), a first-element-initial reduction can differ from small n on (first miss ramp n = 17; re-review S12);
   C++ `numpy_mean` reproduces it bit for bit at every n
   (`CalibrateGross.NumpyMeanIsNumpysMeanAtEverySize`, 20 numpy-printed cases up to n = 40000; model = numpy on 400
   random series).
3. `nav --calibrate-gross` / `gross_match: pm6-6-engine`: PM6-6 in one load and one published NAV; the published
   directory is the plain run's at the final L plus summary.json calibration.
4. (fix round 1) ccache stale-PCH trap: a ccache-restored clang PCH embeds the include mtimes of the `deps/<preset>`
   fetch that first built it; after `deps/<preset>` is deleted and fetched again every PCH consumer is refused ("...
   has been modified since the precompiled header was built"). Delete the two `cmake_pch.cxx.{pch,obj}` (atx-engine,
   atx-impl-core) by name and rebuild with `CCACHE_RECACHE=1` (p9-c2-i then -j; -l used it from the start).

## Fix round 1 (review `4545cec4` at `08297183`: R1, R2, R3, S1-S7; Rulings C2-R2, C2-R3, C2-4b, C2-6, C2-7)

**Outcome: DONE.** Every Required item fixed with a test; S1-S4 and S7 fixed in code, S5 and S6 in this report. Resumed
from `handoff-C2.md` (head `3eca79bf`): the C++ part was committed unbuilt in `c9024beb`; the Python part was applied
from `c2-fix1/` (then removed: the code is in the tree, the patch is in git history) with one correction found on
review (below, R2). Fix-round commit: the one after `3eca79bf` (this report rides in it).

| item | status | change | evidence |
|---|---|---|---|
| R3 (C2-R3) `numpy_mean` at every n | fixed | `strategy_nav_calibrate.cpp`: `sum = 0.0; sum += pairwise_sum(block)` per 8192-value block (`reduce_buffer` = numpy's NPY_BUFSIZE), then `/ n`; header doc | `CalibrateGross.NumpyMeanIsNumpysMeanAtEverySize` (replaces `...PairwiseMean`): 20 bit patterns, ramp and hash series, n = 5 .. 40000 across the 8 / 128 / 8192 boundaries; printed by `c2-numpy-mean-fixture.py` (numpy 1.26.4; re-run this round: the same 20 lines, model vs numpy 0 of 400 random series differ); the unblocked mean fails ramp 8193 and hash 40000. Ledger candidate 2 amended above |
| R1 ref inherits `--calibrate-*` | fixed | `calibrated_doc` (the run recipe) pins `ref.flags` = the cell's own nav.flags and `ref.leverage` = its L unless the ref sets them; a template that sets its own ref gets `change.set ref.flags / ref.leverage` (after its `ref`, dict order) | `test_engine_run_recipe_pins_the_reference_to_the_uncalibrated_construction`: on `scripts/specs/v8/lib-v80.json` the ref's (rule, leverage, flags) as `research_cycle.ref_step` merges them equal the cell's, no `--calibrate-*`, L 1.1474, while the cell's nav ends with the four calibrate tokens; the template branch pins likewise and a template without its own ref gets no pin. By reading: the ref binding is argv-only (`cycle_resume.write_binding`), so where the ref phase actually ran in the run stage, the round-0 recipe's ref argv (calibrate flags) would also have been refused on resume by `check_binding`; now it is the run stage's argv |
| R2 (C2-R2) cell of record / next_parent | fixed | engine mode writes two specs: the run recipe `<cell>-cal.json` (= `calibrated_doc`; name `<name>-gm`, NAV `<output>-gm`; locked, committed, run once `--stop-after nav`) and, after the checks, the cell of record `<cell>-gm.json` = `record_doc` = `WS.gm_doc(cell, L')` with nav.output = the engine NAV (`engine_nav_output`): plain, at the published L', no `--calibrate-*`; locked and committed. Same name, so the same cycle dir (checked: the stage refuses if the two specs name another NAV or cycle dir). match returns `cell_spec` = the run recipe (verify's binding check and judge's `research_cycle run` need the spec the NAV was made under, C2-6) plus `record_spec`, `record_spec_digest`, `record_commit`. Cross-lane, `scripts/wave_result.py` `build()` (E2's file, 3 lines + the layout doc): `cell.spec` / `cell.spec_digest` = the record's when present, and `cell.run_spec` / `cell.run_spec_digest` added only then (Python mode: the same keys and bytes as before). next_parent follows `cell.spec`. Correction to the saved patch: the record's nav.output was the PLACED NAV path (`w.outputs()["nav"]`), which `Cycle.placed` would prefix with `out_root` a second time; it is now the spec-level `<output>-gm` both specs share | `test_engine_gross_match_is_the_python_match_in_one_nav` (now dSR 0.05, accepted): engine and Python cells equal on leverage, gross, gross_parent, gross_calibration, leverage_calibration, corrected AND spec (`lib-w1b-gm.json`); the engine -gm is plain at L' (!= 1.1474), its nav equals the Python -gm's but for output; `run_spec` = `lib-w1b-cal.json` at 1.1474 with `--calibrate-gross repr(G_parent) --calibrate-tolerance 0.005`; spec digests are the two files'; match receipt `cell_spec` = the recipe; judge ran `run lib-w1b-cal.json`; Python-mode `cell` has no run_spec keys; both trees clean. Then a child template on each mode's next_parent (`RC.load_spec`): nav.leverage equal (= L') and nav.flags equal, no `--calibrate-*` |
| C2-4b match pins | fixed | engine run stage records `spec_digest`, `w_combined` (`<ic.w_output>-<n>/train_combined.json` of the attempt `Cycle.ic_attempt` picks: what the NAV step reads) and `w_combined_sha256`; it refuses a run with no complete w output. `match_inputs` pins all three in engine mode (STALE on change); Python mode unchanged | `test_engine_match_pins_the_w_output_its_nav_reads`: `--until match`, the recorded digests equal the files', rewrite `train_combined.json`, run again: exit 3 `STALE: run recorded w_combined_sha256`, no verify receipt. Fixture: `FakeCycle --stop-after w` writes `<w base>-1/summary.json` (complete) and `train_combined.json` |
| C2-6 | applied, documented | verify and judge keep running the run recipe (`-cal`); `cycle_resume.check_binding` (E2's file) is NOT changed. A `research_cycle run` of the plain `-gm` refuses the calibrated NAV dir (its spec digest and argv are not the ones that made it): correct, "the argv that made it" is the binding invariant | asserted in the R2 test (judge's call) |
| C2-7 | applied, documented | the engine NAV dir keeps the requested-L token of the cell's name (`out/nav-w1b-L1.1474-gm` while L' is another value); the published L' lives in summary.json `calibration.leverage` and in the cell of record's nav.leverage (`engine_nav_output` docstring) | R2 test asserts the dir name |
| S1 100 columns | fixed | nav_replay.cpp line, both CMake comment tails, one Err line in calibrate.cpp | built /W4 /WX |
| S2 reuse exclusions | fixed (partly) | `CalibrateGross.ReuseAndItsExclusionsPublishThePlainRunAtTheFinalLeverage` (strategy_nav_replay_test.cpp): X-5's construction keys (price-risk-v1 neutralization, locate-in-aim, delta order basis, liquidity cache) -> `construction_reused` true; `--adv-hold-q .5` -> false; hold band .5 + `--emit-holdings` -> false; each case one correction and byte-identical to the plain run at L' (summary.json with calibration dropped; the holdings dir too) | green in Debug (6.9 s) and Release. Not covered: two-speed (needs saved sleeves in the fixture), the `--max-bytes` budget case (the loader's own charge is not exposed), warm start (the fixture's score_begin is 0), capacity with X-5 keys (OneCorrectionMatchesTwoProcess covers capacity) |
| S3 stats_ext wording | fixed | the comments now claim the same bits for the polynomials only and name the remaining dependence (a folded call rests on the compiler's exp/erfc/log/sqrt agreeing with the CRT's, as the 7 pinned points show) | comment only |
| S4 one leverage constant | fixed | `aim_leverage_min/max` in `strategy_target_replay.hpp`, used by `validate_config` and by `nav_calibrate_leverage_min/max`; `risk_target::leverage_cap()` keeps its JSON integers 1 / 2 (a double prints `1.0` and would move the `--list-rules` bytes) | RefusesOutsideRange, NavBookRule.* green |
| S5 report wording | fixed | root's identity step 3 reworded (below) | - |
| S6 tape vs `--max-bytes` | fixed (note) | root reads `calibration.construction_reused` in the X-5 gm pair: with cadence 1 the tape is about sessions x names x (8 + 2) B and must fit what X-5's `--max-bytes 1073741824` leaves after the reserve and the load; false there means pass 2 formed the construction again (same bytes, smaller wall gain) | root merge item 2 |
| S7 capability pin | fixed | `NavBookRule.AimLeverageIsOptionalWithItsDefault` checks `calibrate-gross-pm6-6` in `nav --list-rules` capabilities | green |

### Builds and tests

All builds `scripts/research-build.ps1` in pool-15, one at a time after the host was idle and the gate admitted
(`c2-fix1-gate-<t>.log` in this dir; receipts `build-equity/mega-p9-c2-<t>-receipt.json`):

- **p9-c2-h** (equity-dev, `atx-impl-strategy-target-tests` + `atx-equity-strategy-targets`, source `3eca79bf`, the
  2 dirty entries were the Python files only): exit 0, 724 s, 307 TUs, 10 links; no first-party warning (two
  databento `getenv` deprecations).
- **p9-c2-i** (equity-rel, target tests): exit 1 after 29 s, NOT a code defect: ccache restored both PCHs
  (`cmake_pch.cxx`, atx-engine and atx-impl-core: `direct_cache_hit`) from round 0, and a clang PCH embeds the mtimes
  of its includes; `deps/equity-rel` had been deleted (DISK) and fetched again, so clang refused every consumer
  ("Eigen/Dense has been modified since the precompiled header was built"). Fixed in place: the two restored
  `.pch` / `.obj` deleted by name, then
- **p9-c2-j** (equity-rel, target tests, `CCACHE_RECACHE=1`: every TU compiled and its cache entry refreshed):
  exit 0, 1686 s, 248 TUs (the 69 good round-0 hits of -i kept), 9 links, 3 jobs; no first-party warning. The 5 dirty
  entries are the Python / sprint files of this round (no C++ file was dirty: the C++ is `c9024beb`).

gtests (logs `c2-fix1-gtest-<t>-{named,whole}.log`): Debug (h) `CalibrateGross.*:NavBookRule.*:BookNormalScore.*`
**19/19** (the two M1c-RED tests `TiesShareTheMeanRankAndMirrorsAreOpposite`, `FixtureTellsWrongRulesApart` and
`NormPpfOfALiteralIsItsRunTimeValue` included), whole binary **348/348**; Release (j) the same filter **19/19** (the two M1c-RED reds green in Release), whole binary
**348/348**. No expected value edited.

pytest (Python 3.12, explicit paths, `-p no:cacheprovider`, own `--basetemp`): `scripts/tests/test_research_wave.py`
**19 passed** (17 before: one engine test rewritten for R2, two new for R1 and C2-4b); cwd `scripts/tests`:
`test_no_python_mirror.py test_wave_driver.py test_wave_hardening.py test_wave_queue.py test_wave_review.py
test_wave_review_minor.py test_wave_scoreboard.py test_wave_speed.py` **77 passed** (G-P5 line unchanged: 37 mirrored
functions in 14 rows).

### Files (fix round 1)

`atx-impl/src/strategy_nav_calibrate.{hpp,cpp}`, `atx-impl/src/strategy_target_replay.{hpp,cpp}` (S4),
`atx-impl/src/strategy_nav_replay.cpp` (S1), `atx-impl/CMakeLists.txt` and `atx-impl/tests/CMakeLists.txt` (comment
wraps), `atx-engine/include/atx/engine/eval/stats_ext.hpp` (S3, comments only), `atx-impl/tests/strategy_nav_calibrate_
test.cpp`, `atx-impl/tests/strategy_nav_replay_test.cpp`, `atx-impl/tests/strategy_vol_target_test.cpp`;
`scripts/wave_stage_cell.py`, `scripts/wave_result.py` (cross-lane, E2), `scripts/tests/wave_fixture.py`,
`scripts/tests/test_research_wave.py`; sprint dir: `c2-numpy-mean-fixture.py`, the gate / gtest logs, this section,
`handoff-C2.md` (pointer).

### Root merge items (replace "How root verifies" above where they differ)

1. Build equity-dev and equity-rel: `atx-impl-strategy-target-tests`, `atx-equity-strategy-targets`,
   `atx-engine-eval-tests`, `atx-engine-book-tests`, `atx-impl-tests` (its glob compiles strategy_nav_calibrate_test.cpp;
   not built by the lane). gtests as listed above, `BookNormalScore.*` green in Release. If `deps/<preset>` was fetched
   again since that tree's last PCH build, expect the stale-PCH refusal of p9-c2-i (rebuild with `CCACHE_RECACHE=1`
   after deleting the two `cmake_pch.cxx.{pch,obj}`).
2. The X-5 gm-pair identity run: `nav <X-5 cell argv at L> --calibrate-gross <repr(G_parent)>` vs the two-process
   `-gm` NAV: every file byte-identical except summary.json `/calibration` (added) and `/producer` if the exe differs;
   `calibration.corrected`, `.leverage` (the -gm spec's L' as a double) and `.gross` (= the -gm NAV's mechanics G).
   Read `calibration.construction_reused` (S6) and the wall before / after. Then the engine wave (a manifest with
   `"gross_match": "pm6-6-engine"`, an X-5 replay of a corrected cell): compare with the Python mode's wave-result
   `cell` on leverage, gross, gross_parent, gross_calibration, leverage_calibration, corrected and the NAV bytes;
   `cell.spec` is the plain `-gm` in both modes (engine: plus `run_spec` = the `-cal` recipe); `nav`, the NAV dir name
   (C2-7), `spec_digest`, `commit` and `readers` differ by design (S5).
3. Only after that identity: delete the Python gross-match rule (`wave_rules.py:110-119` at base `ad406718` =
   `matched_leverage` and `gross_matches`, lines 113-122 at the lane head), G-P5. Its callers go with it: the Python
   `match` path in `wave_stage_cell.py` and `FakeCycle.calibrate` / the Python-mode wave tests (which then need their own
   copy of the 4-decimal rule or the engine fixture).
4. Keep-both merges: E2 also edits `scripts/wave_stage_cell.py` and `scripts/wave_result.py`; COV shares the
   `atx-impl/tests/CMakeLists.txt` tail. Root check C2 (ledger): no P9 reference was produced under the `rel` preset.
5. Ledger: candidates 1 and 2 (2 as amended), 3; plus the PCH trap above (a ccache-restored clang PCH embeds the
   include mtimes of the deps fetch that built it; deleting and re-fetching `deps/<preset>` makes the next build
   refuse every PCH consumer until the PCH is compiled again).

### Open

- `WS.gm_doc`'s template branch does not pin a template-set ref at the cell's L (the plain branch does); the cell of
  record inherits that (Python parity, C2-R2). No template in `scripts/specs` sets a ref, so nothing runs it today.
  (Fixed for the cell of record in fix round 2, S8; the Python mode's `gm_doc` keeps it.)
- The cell of record's digest is recorded at match time; no later stage re-pins it (verify pins the run recipe's).
  The file is committed by the stage, and every later commit requires a clean tree, so an edit would show as dirty.
  (Superseded by fix round 2: a COMMITTED edit passed; R4 pins it now.)

## Fix round 2 (re-review `4df09dee` at `e68adf41`: R4, R5, S8-S13; Rulings C2-8, C2-9)

**Outcome: DONE.** Both Required items fixed with tests; S8, S9, S11, S12 fixed; S10 and S13 not (reasons below).
Code commit `bb75326d` (committed before the builds, so their receipts are of a committed source); this report, the
logs and the ledger-candidate edits ride in the next commit. Ledger candidates above: 3 moved back into the list (fix
round 1 had inserted its section inside it), 2 reworded (S12), 4 added (the PCH trap).

| item | status | change | evidence |
|---|---|---|---|
| R4 cell of record unpinned after match | fixed | `scripts/wave_stage_record.py` `verify_inputs` (E2's file; accepted cross-lane edit, Ruling C2-8; 2 lines outside E2's hunks): in engine mode it also pins match's `record_spec_digest` against the `-gm` file now, so verify and every resume refuse a cell of record that changed after match; Python mode adds no key (its `-gm` is `cell_spec`, already pinned) | `test_engine_cell_of_record_is_pinned_after_match`: the reviewer's sequence (`--until match`, a committed edit of `lib-w1b-gm.json` nav.leverage to 1.9999, `run`) exits 3 `STALE: match recorded record_spec_digest`, no verify receipt, no wave result; the unedited path exits 0 with verify's input `match.record_spec_digest` = match's = the file's digest, and wave-result `cell.spec_digest` = that digest, next_parent = the record. Without the two lines the test fails (checked by reverting them in place, then restoring) |
| R5 warm start under reuse | fixed | `CalibrateGross.ReuseAndItsExclusionsPublishThePlainRunAtTheFinalLeverage` gains case `x5-keys-warm-start`: the X-5 keys on `artifact_from(dir, p, 30)` with `--warm-start-sessions 18` (decisions from row 12). It asserts the contract, not the implementation: the calibrated run is the plain warm run at L' byte for byte (every file; summary.json but `calibration`), `corrected` true, `construction_reused` true, summary `warm_start.sessions` 18 / `score_begin_row` 30, the S2 daily CSV has the scored rows only. Every case (all four) now also pins the calibration record to the plain runs bit for bit: `gross_requested` = the plain run at L's all-rows G, `leverage` = L' (4 decimals of L x G_P / G), `gross` = the plain run at L''s G | green in Debug (k) and Release (l) |
| S8 template-set ref in the record | fixed | `record_doc`'s template branch sets `change.set ref.leverage` = the cell's L when the resolved spec has a template-set ref without its own leverage (as `calibrated_doc` does); no template sets one today | R1 test: the record's sets carry `nav.leverage` L', `ref.leverage` L after the template's `ref`; a template without a ref gets only `nav.output` / `nav.leverage` |
| S9 tape capacity vs `--max-bytes` | fixed | `strategy_nav_replay.cpp`: `reserve_construction` reserves the record's vectors at `construction_tape_bytes`' bound once when reuse is chosen (one `construction_decisions` helper now feeds both the bound and the reserve), so capacity cannot exceed the budget by a growth factor or a reallocation copy; values untouched (a failed reserve is caught and refused like any allocation) | the reuse cases of the test above and TieredRunReusesTheConstructionByteForByte green in both builds; no gtest of the budget branch (Ruling C2-8) |
| S10 two-speed exclusion untested | not fixed | Ruling C2-8: coverage stays as reported; no planned P9 cell sets `--two-speed` (reviewer's table) | - |
| S11 test-side copy of the ref merge | fixed | the R1 test builds the ref command through `research_cycle.Cycle.ref_step` itself (pins filled from the spec's inputs; `cycle_resume.step_args`, what the argv-only ref binding hashes): the run recipe's ref argv equals the cell's, carries no `--calibrate-*` and L 1.1474; the round-0 shape (no ref pins) does carry the calibrate flags | pytest |
| S12 candidate 2 wording | fixed | "can differ from small n on (first miss ramp n = 17)" | - |
| S13 exe-notes wording | not fixed | E2 rewords the Python branch's line in its own lane; aligning the engine branch here would conflict with E2's hunk: root aligns it at the keep-both merge (root item 4) | - |

### Builds and tests (round 2)

`scripts/research-build.ps1`, pool-15, one at a time after the host was idle and the gate admitted (`c2-fix2-gate-<t>.log`,
receipts `build-equity/mega-p9-c2-<t>-receipt.json`); both trees configured again first through
`scripts/atx-build.ps1 configure -Preset <preset>` (round 1 had deleted them):

- **p9-c2-k** (equity-dev, `atx-impl-strategy-target-tests` + `atx-equity-strategy-targets`, source `bb75326d`):
  exit 0, 289 s, 307 TUs, 10 links. DirtyEntries 1 = this report (S12 edit), no code file.
- **p9-c2-l** (equity-rel, target tests, source `bb75326d`, `CCACHE_RECACHE=1` from the start: `deps/equity-rel` was
  fetched again, so the PCH trap above would have fired): exit 0, 772 s, 306 TUs, 9 links, 4 jobs; no first-party
  warning. DirtyEntries 1 = this report.

gtests (logs `c2-fix2-gtest-<t>-{named,whole}.log`): Debug (k) `CalibrateGross.*:NavBookRule.*:BookNormalScore.*`
**19/19**, whole binary **348/348**; Release (l) the same filter **19/19** (both M1c-RED tests green), whole binary
**348/348**. No expected value edited.

pytest (explicit paths, `-p no:cacheprovider`, own `--basetemp`): `scripts/tests/test_research_wave.py` **20 passed**
(+1: R4; R1's test rewritten for S8 / S11); cwd `scripts/tests` the 8 wave suites **77 passed** (G-P5 line unchanged).

### Root merge items (additions to fix round 1's list)

1. Build from a clean tree at the merge SHA (the lane receipts carry 1 dirty sprint-doc entry; not merge evidence).
2. Cross-lane edits to place: `scripts/wave_result.py` (round 1) and `scripts/wave_stage_record.py` `verify_inputs`
   (round 2, R4: the `record_spec_digest` pin; outside E2's hunks). Align the engine branch's exe-notes log line in
   `wave_stage_cell.py` with E2's new wording at the keep-both merge (S13).
3. X-5 gm-pair identity run as listed in fix round 1, item 2: X-5 carries `--warm-start-sessions 60` and
   `--max-bytes 1 GiB`, so it is also the real-data cover of R5 and S9; read `calibration.construction_reused`.
4. Ruling C2-9 (with the deletion, not this lane): once root has run the identity and deleted the Python gross-match
   rule, a manifest that needs gross matching and does not name `gross_match` is refused at plan with a message naming
   the key and `pm6-6-engine`; no silent default; every P9 template names it; completed v1 waves are not re-run
   through the driver (C3 carries the refusal if root's deletion commit cannot hold it).
5. Ledger: candidates 1-4 above.
