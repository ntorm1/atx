# Lane C2 hand-off (owner stop, 2026-10-03)

> RESUMED and completed after the owner's re-issued goal: see `task-C2-report.md` (this dir) for the outcome,
> commits and root's verification list. The text below is the stop-time design record, kept as written.

- Pool `C:/atx-wt/pool-15` (reused; no lease call). Branch `feat/p9-c2-20261003`, base `ad406718` (all eight wave-1
  lanes merged, C1 included). Head: the `wip(c2)` commit that adds this file and `c2-ppf-fma.py` (no code yet).
- Status: research and design done; **no source file edited**. About 25 % of the lane.
- Processes: none started except one short Python run (finished). Nothing left running.

## Items

| item | state |
|---|---|
| M1c-RED (`BookNormalScore` Release-only 1-ULP) | **root cause found and reproduced** (below); fix designed, not written |
| `nav --calibrate-gross TARGET --calibrate-tolerance .005` (C++) | designed (below), not written |
| gtests `CalibrateGross.WithinToleranceOnePass`, `.OneCorrectionMatchesTwoProcess`, `.RefusesOutsideRange` | not started |
| match stage calls it, behind a wave-manifest key (P15; cross-lane `scripts/wave_stage_cell.py`) | designed, not written |
| `wave_rules.py:110-119` deletion | NOT this lane's step: delete only after root's identity run (G-P5, plan §0.6) |
| carried C1 minors (null risk-store check, risk-target param ranges, adv-hold+capacity 5 lockstep passes, K-P9-7 `aim_leverage` required-but-defaulted, C1-SPO gtest pins) | not started |
| report `task-C2-report.md` | not started |

## M1c-RED: root cause (reproduced bit for bit)

Kernel `atx-engine/include/atx/engine/book/normal_score.hpp` calls `eval::norm_ppf` (`atx-engine/include/atx/engine/
eval/stats_ext.hpp:97-129`, Acklam + one Halley step). The four Horner polynomials are written as single expressions
`(((a0*r + a1)*r + a2)...)`. clang-cl's default FP model (/fp:precise = `-ffp-contract=on`) emits each `acc*r + c` as
`llvm.fmuladd`. At run time on the x86-64 baseline (no FMA: equity-dev and equity-rel have no /arch:AVX2,
`CMakePresets.json:48-55`) fmuladd lowers to mulsd + addsd (two roundings). In Release (/O2) the test's expected value
`norm_ppf(10.0 / 12.0)` has a constant argument, is inlined and **constant-folded**, and LLVM folds fmuladd as a
**fused** multiply-add (one rounding). The Acklam starting point then differs in its last bit, and that survives the
Halley step. The kernel's u is runtime (never folded), so the kernel gets the two-rounding value. In Debug the test TU
is /Od (`atx-impl/tests` TUs are not in the Debug /O2 list), so nothing is folded and both sides match.

Not the cause: x87 (x64 uses SSE2), the CRT (Debug/Release ucrt), u itself (10/12 is the same double both ways).

Evidence: `c2-ppf-fma.py` (this dir) evaluates norm_ppf with separate multiply/add and with exact fused multiply-add
(Fraction), using the UCRT exp/erfc/log through Python 3.12 (MSC). Output:
`10/12` separate 0.96742156610170082, fused 0.96742156610170071, the exact pair root reported (kernel ...082 vs
expected ...071). `1001/1002` separate 3.0908256489420887, fused 3.0908256489420891 (root: top ...887 vs ...891).
`2/12`, `5/12`, `8/12`, `1/4` are the same both ways, which is why those EXPECT_EQs passed. blom `(n-.375)/(n+.25)` and
`(n-.5)/n` also differ (they are only used in EXPECT_GT there).
Run: `"C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-p9-20261003/c2-ppf-fma.py`.

Fix (designed, not written): make norm_ppf contraction-free. Add to `stats_ext.hpp` `detail`
`inline f64 mul_add_unfused(f64 acc, f64 x, f64 c) noexcept { const f64 product = acc * x; return product + c; }`
(the named product is its own full expression, so `-ffp-contract=on` cannot form fmuladd; no `contract` flag is set
under "on", so the backend does not fuse either) and write the four polynomials as nested calls of it. No pragma
(`#pragma clang fp contract(off)` would also work but is clang-only). Leave the test's expected values as they are:
the unchanged test passing in Release is the acceptance. Optionally add a test
`BookNormalScore.NormPpfOfALiteralIsItsRunTimeValue` (literal vs a `volatile`-read argument at 10/12, 1001/1002,
blom, over_n, a lower-tail p) in `book_normal_score_test.cpp`.

Identity of the fix: every run-time evaluation on a non-FMA build is unchanged bit for bit (lowered fmuladd is exactly
mul then add). Only compile-time folds and /arch:AVX2 (`rel-avx2`) run time move, to the run-time value. Checked: of
the Debug /O2 TUs (`atx-impl/CMakeLists.txt:115-129, 211-223`, `atx-engine/CMakeLists.txt:203-224`) only
`strategy_target_replay.cpp` reaches norm_ppf, through normal_scores with a runtime u. So no published research byte
in equity-dev or equity-rel moves. The edit is cross-lane (engine eval header; norm_ppf is also used by
deflated_sharpe, haircut, min_trl, decay_monitor and strategy_mine_rule): list it.
Root confirms: Debug and Release (`build-equity-rel`) `atx-impl-strategy-target-tests
--gtest_filter=BookNormalScore.*:NormScore.*`, plus engine `atx-engine-eval-tests --gtest_filter=*StatsExt*:*Deflated*:*MinTrl*`
and `atx-engine-book-tests --gtest_filter=BookNormalScore.*:*DecayMonitor*` on both builds.

## --calibrate-gross: design

Read: `strategy_nav_replay.{hpp,cpp}` (run_nav_replay :3327-3428, run_nav_grid :3430-3555, dispatch :3569-3772,
replay_books :2391-2514, run_books :1310-1437, decide_construction :897-913, plan_decision :1031-1084, nav_recipe
:1706-1769), `strategy_nav_v7.{hpp,cpp}` (dispatch_nav_v7 :1325, extension state, run_records), the C1 report.

1. **Flags**, parsed in `dispatch_nav_replay` (v7 runs reach it through `dispatch_nav_v7`'s re-dispatch, so one
   parser covers both): `--calibrate-gross G_P` (finite > 0) and `--calibrate-tolerance T` (default .005, finite > 0;
   only with `--calibrate-gross`). Refuse with `--construction-grid` and with an spo rule (`v7::shared_plan_state()`:
   `spo::Engine::begin_run` does not clear rows or calibration, so pass 1 would pollute the published spo files).
2. **Pass 1**: one load (`admit_and_load` once), replay the primary S2 book only (`scenarios[nav_primary_scenario_index]`,
   no capacity books, no observer), G = mean of its `days[].gross_leverage` over every row, computed as numpy does
   (pairwise sum: blocks of 8 accumulators, PW_BLOCKSIZE 128, split n/2 rounded down to a multiple of 8; then / n), so
   G equals `nav_summ.construction_stats` `mean_gross_leverage_all_rows` bit for bit (the CSV prints 17 digits, so the
   values round-trip). G_P is passed as Python `repr(float)`, parsed exactly.
3. **Decision** (PM6-6): |G - G_P| <= T: L stays. Else L' = L x G_P / G rounded to 4 decimals exactly as Python
   `f"{x:.4f}"` then parsed (use `std::to_chars(..., fixed, 4)` then `from_chars`); refuse (InvalidArgument, nothing
   published) when L' is outside [1, 2] (`strategy_target_replay.cpp:126-134`).
4. **Pass 2**: the full lockstep (main + capacity books, holdings observer if any) at the final L, published as today.
   Then G2 from pass 2's S2 book; record `matched = |G2 - G_P| <= T`.
5. **Record**: put `calibration {target, tolerance, leverage_requested, gross_requested, corrected, leverage,
   gross, matched, rule}` in **summary.json, not recipe.json** (deviation from the brief's "recipe", C1 precedent: the
   decide path `load_nav_deploy` :3808-3840 recomputes the recipe SHA from argv, and recipe.json then stays
   byte-identical to the two-process `-gm` run's). Root's identity: every file byte-identical to the two-process
   matched NAV except summary.json at `/calibration` (added) and `/producer` (only if the exe differs).
6. **Cached desired targets** (the efficiency the brief asks for): an optional `ConstructionTape*` in `run_books`
   (nullptr = today's path, byte for byte). Record mode stores per decision {t, cadence, rebalance, census,
   construction, shared.desired (cadence decisions), tiers.tier/missing (tiered)}; replay mode restores them instead
   of `decide_construction`, refusing (Internal) on a t/cadence mismatch. Valid only when the construction reads
   nothing that differs between the passes: off with `adv_hold_q > 0` (cap reads L and the NAV multiple), with
   two-speed-v1 (F at L; `plan_decision` reads `shared.state.fast_before`), and with a holdings observer under a hold
   band (`emit_holdings` reads `shared.hold_in`); then pass 2 recomputes. `form_desired` reads `aim_leverage` only for
   adv-hold and two-speed (`strategy_target_replay.cpp:490, 651`). Charge the tape (decisions x names x (8 + 2) B)
   against `--max-bytes` (it is in the recipe, so it cannot be raised without moving recipe bytes); if it does not
   fit, run without the tape. Within tolerance the published run is the pass-2 replay at L (one calibration pass, no
   correction); `WithinToleranceOnePass` should pin "no correction, output = the plain run at L".
7. Put the calibration logic in a new `atx-impl/src/strategy_nav_calibrate.{hpp,cpp}` (pairwise mean, PM6-6 step,
   range check, summary block) and add it to `atx-impl-core` at the tail of `atx-impl/CMakeLists.txt` (C2 owns that
   file in wave 2). New gtests can go in a new `atx-impl/tests/strategy_nav_calibrate_test.cpp`, appended to
   `atx-impl-strategy-target-tests` (COV shares that list: append one line at the tail).

## Match stage integration: design (cross-lane, not written)

- Key: a new `GROSS_MATCH` entry in `scripts/wave_rules.py` (e.g. `"pm6-6-engine": {"tolerance": .005,
  "corrections": 1, "engine": True}`). `wave_manifest.validate` already checks `gross_match` against `GROSS_MATCH`, so
  no `wave_manifest.py` edit (E2's). Absent / `pm6-6` = today's Python match stage.
- Engine mode: the run stage (`wave_stage_cell.run_stage`) stops before the NAV (`--stop-after w`; check that every
  cell spec has a w phase, else use the phase before nav); the match stage reads the parent's mechanics (G_P),
  writes `<cell>-gm.json` = the cell spec with `nav.flags += ["--calibrate-gross", repr(G_P), "--calibrate-tolerance",
  "0.005"]` and a new `nav.output`, locks, commits, runs `research_cycle run <gm> --stop-after nav`, reads
  summary.json `calibration` plus the mechanics reader (g_cell), raises StageError when `matched` is false, and
  returns the same dict keys `wave_result.py:36-47` and `wave_stage_record.py` read (`corrected`, `leverage`,
  `g_parent`, `g_calibration`, `leverage_calibration`, `g_cell`, `nav`, `cell_spec`, ...). `research_cycle.nav_step`
  passes `nav.flags` verbatim (`research_cycle.py:1387`).
- Open: the run stage change is in the same file but changes the run stage's output (`nav` absent);
  `match_inputs`/`run_inputs` resume pins need a look.

## Decisions so far

- `atx-impl/src/config.{cpp,hpp}` is the pipeline config, not the NAV config: not touched.
- Calibration record in summary.json (C1 precedent), not recipe.json.
- spo rules refused with `--calibrate-gross`.
- Engine-mode key as a `gross_match` value, not a speed key (speed keys default true; P15 needs absent = off).

## Open questions for the PM

1. Accept the calibration record in summary.json instead of recipe.json (deploy-path recipe pin, identity)?
2. Engine mode stops the run stage before the NAV: a cell then has no NAV at the requested L. Acceptable?
3. If pass 2 is not within tolerance: publish and record `matched: false` (the match stage stops), as the
   two-process flow did?

## Traps

- `max_working_bytes` is a recipe key: raising `--max-bytes` for the tape moves recipe bytes.
- The v7 extension accumulates `tc`, `books` and scaler records across replays in one process (`observe`, `capture`);
  publication uses `run_records` from the run's own books, but only call `capture` for pass 2.
- `dispatch_nav_v7` re-dispatches into `dispatch_nav_replay` with its own token list: unknown flags there are usage
  errors, so the new flags must pass through `parse_nav_v7_args` untouched (it forwards unknown tokens).
- Do not open any NAV output (summary.json, CSVs, stage_seconds) of 2020-2023; receipts and manifests only.
- Lanes do not build; re-read every C++ line for /W4 /WX (sign conversion, shadowing, unused).
