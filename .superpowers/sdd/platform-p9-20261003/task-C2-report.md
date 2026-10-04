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
2. numpy's float64 `mean` = (0.0 + pairwise_sum) / n (8 accumulators, 128 block, split n/2 down to x8); a
   first-element-initial reduction differs; C++ `numpy_mean` reproduces it bit for bit.
3. `nav --calibrate-gross` / `gross_match: pm6-6-engine`: PM6-6 in one load and one published NAV; the published
   directory is the plain run's at the final L plus summary.json calibration.
