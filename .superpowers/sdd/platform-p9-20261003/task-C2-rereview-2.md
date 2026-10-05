# Lane C2 re-review 2 (scoped: fix round 2)

## Verdict

**APPROVE** (0 Required, 1 Suggested). R4 and R5 are closed; S8, S9, S11 and S12 are fixed as claimed. S10 and the `--max-bytes` budget test stay as reported (Ruling C2-8). S13 is a root merge action, spelled out below. M1c-RED is untouched, and flag-absent identity holds.

## Reviewed SHA

`4d77d3db` on `feat/p9-c2-20261003` (pool-15). Code commit `bb75326d`; `4d77d3db` holds the report and logs only.

I diffed `e68adf41..4d77d3db` without my review commit `4df09dee`, which touches only sprint docs. The code part is 5 files: `strategy_nav_replay.cpp`, `strategy_nav_replay_test.cpp`, `test_research_wave.py`, `wave_stage_cell.py` and `wave_stage_record.py`.

HEAD matched, and `git status --porcelain` showed 0 lines before and after the review.

## Evidence (run by the reviewer)

1. **Lane pytest re-run.** Command:
   ```
   PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider --basetemp <scratch>/c2rr2-bt4 C:/atx-wt/pool-15/scripts/tests/test_research_wave.py
   ```
   Run from cwd pool-15. Output: `20 passed in 33.52s`, exit 0.

2. **R4 attacks** (scratch `c2rr2_attack_test.py` on the lane's `wave_fixture`, synthetic, engine mode unless stated): `6 passed`. Each committed edit sets `nav.leverage` to 1.9999 and is written with LF.

   | Case | Setup | Result |
   |---|---|---|
   | A | `--until match`, then edit `lib-w1b-gm.json`, then `run` | exit 3, `STALE: match recorded record_spec_digest 68169664...`; no verify receipt, no wave-result |
   | B | `--until verify`, then edit `-gm`, then `run` | exit 3, same STALE; no judge receipt, no wave-result |
   | C | edit the run recipe `lib-w1b-cal.json` after `--until match`, and again after `--until verify` | exit 3, `STALE: match recorded spec_digest ...`; no wave-result |
   | D | NAV binding corrupted, so verify fails | exit 4, `07-verify.failed-1.json` |
   | D, continued | binding restored, `-gm` edited and committed | exit 3, record_spec_digest STALE |
   | D, continued | `-gm` restored byte for byte and committed | exit 0; wave-result `cell.spec_digest` equals the file's digest |
   | E | Python mode `pm6-6`, full run | verify inputs are `match.readers`, `match.spec_digest` and `previous_receipt_sha256`; no `record_spec` key in verify inputs or match outputs (unchanged layout) |
   | F | residual: the `-gm` edited and committed *inside* the same run, while judge's `research_cycle run lib-w1b-cal.json` executes | exit 0; wave-result written with `cell.spec_digest` not equal to the file's digest. Becomes S14 |

3. **Merge with E2's newest head `d0054b32`** (`git merge-file`; common base `1239a5ff`, whose `wave_stage_record.py` equals `ad406718`'s): 0 conflicts in all seven shared files.
   - Files: `wave_stage_record.py`, `wave_stage_cell.py`, `wave_result.py`, `wave_steps.py`, `wave_fixture.py`, `test_research_wave.py`, `wave_rules.py`.
   - In the merged `verify_inputs` (merged lines 283-292), C2's two lines sit after E2's unchanged `spec_digest` pin.
   - E2's round on `nav.leverage` preflight and its raw-rule-flag guard do not touch `verify_inputs`.

4. **Receipts and logs.**
   - `mega-p9-c2-k` (equity-dev) and `-l` (equity-rel): Source `bb75326d`, exit 0, 307 / 306 TUs.
   - **DirtyEntries 1, not 0.** `bb75326d..4d77d3db` touches only sprint docs, so the one entry is the tracked report; no code file was dirty.
   - Build logs: no first-party warning; only the third-party databento `getenv` deprecation and clang-cl's `/MP`.
   - `c2-fix2-gtest-{k,l}-named.log`: `CalibrateGross.*:NavBookRule.*:BookNormalScore.*` 19/19 in both, `ReuseAndItsExclusions...` included.
   - `-whole` logs: 348/348 in both.

## Item by item

**1. R4: closed.**
- `verify_inputs` (`scripts/wave_stage_record.py:263-264`) pins match's `record_spec_digest` against the `-gm` file. It runs before verify and on every resume (evidence 2, cases A, B and D).
- An edit of the run recipe is refused by the existing `spec_digest` pin (case C).
- A failed verify does not open a hole: retries are pinned against match's record, not against verify's own receipt (case D).
- Python mode adds no key (case E).
- Test `test_engine_cell_of_record_is_pinned_after_match` (`test_research_wave.py:329`) covers both paths: unedited gives exit 0 with verify input = match digest = file digest = wave-result digest; edited gives exit 3 with no verify receipt and no result. Without the two lines the edited case would run to exit 0, as in my round-1 demonstration, so the test has teeth.
- Residual (case F): a *concurrent* committed edit during the judge stage of the same run is not caught. Python mode has the same class of window from judge's own binding check to record. It needs a second committer while the driver holds the state-dir lock, so it is Suggested (S14), not Required.

**2. R5: closed; the test asserts the contract.** Case `x5-keys-warm-start` (`strategy_nav_replay_test.cpp:3531-3617`):
- Setup: the X-5 construction keys on `artifact_from(dir, p, 30)` with `--warm-start-sessions 18`, so decisions start at row 12.
- All three runs share the warm `execution`: `first` (plain at L), `second` (plain at L') and `calibrated`.
- Contract asserts:
  - every published file except summary.json is byte-identical between `calibrated` and `second`;
  - summary.json without `calibration` is equal;
  - `gross_requested` is bit-equal to `first`'s all-rows G, `leverage` to L', and `gross` to `second`'s G;
  - `warm_start.sessions` 18 and `score_begin_row` 30;
  - the S2 daily has only the scored rows.
- `construction_reused` true is asserted, so reuse is actually exercised.
- It would fail on a cold or stale construction, by reading:
  - A record that missed the warm-up decisions fails `restore_construction`'s `rec.t != t` check, so `calibrated` returns Internal and the ASSERT fails.
  - A wrongly restored warm-up construction moves the books' positions at `score_begin`, so the daily bytes differ from `second`.

**3. S9 C++: no published byte can move.**
- `reserve_construction` (`strategy_nav_replay.cpp:1323-1330`) only reserves capacity. It runs only under `calibrate.on() && reuse` (`:3580-3582`, call at `:3581`).
- The reserved size is exactly `construction_tape_bytes`' bound: `decisions` Decisions, `decisions x names` doubles, plus 2 x `decisions x names` bytes when tiered. The bound was already admitted against `max_working_bytes - held`, so `decisions * names` cannot overflow `usize`.
- Actual decisions (`t + 2 < end` over `[begin - K, end)`) never exceed `construction_decisions` = load sessions + K (`:3488-3490`), so there is no reallocation after the reserve. That function is now shared by the bound and the reserve.
- `tape` lives in `run_nav_replay` scope across both passes.
- A failed reserve throws inside the function's `try` and is refused by `catch (const std::exception&)` (`:3675`) before any output.
- `/W4 /WX`: the k and l builds exit 0 with no first-party warning. Source is `bb75326d`; DirtyEntries is 1 (a sprint doc, not 0; evidence 4).

**4. S8, S11, S12 as claimed; S13 is root's.**
- S8: `record_doc`'s template branch (`wave_stage_cell.py:328-333`) pins `ref.leverage` to the cell's L when a template-set ref lacks it. The pin is appended after the template's `ref`. The test checks the sets and their order.
- S11: the R1 test now hashes `cycle_resume.step_args(Cycle.ref_step(...))`, which is research_cycle's own merge. The round-0 shape (ref unpinned) carries `--calibrate-*`, so the test has teeth. It sets `Cycle.pins` directly, which is acceptable for a test.
- S12: the report wording is fixed (`task-C2-report.md:237`).
- **S13, exactly what root does at the keep-both merge:** in the merged `scripts/wave_stage_cell.py` `run_stage` engine branch (C2 head `:74-76`):
  - replace `log(f"   exe notes (reused on a rebuilt exe without an exes_sha256 pin, E1-REUSE-a2): {'; '.join(notes)}")` with E2's two-line text from the Python branch: `log(f"   exe notes (made by another executable than the one on disk now; judged by the exes_sha256 pin rule, " f"E1-REUSE-a2): {'; '.join(notes)}")`;
  - add E2's comment `# ruling E1-REUSE-a2: outputs made by another exe than on disk now` on its `notes = exe_notes(...)` line.

  No test pins the engine-branch text. E2's `test_wave_driver.py:401` counts the new wording on the Python path only, and `:326` at base, which counts the old wording, is E2's to replace.

**5. E2 merge: PASS** (evidence 3).

**6. Identity and seal: PASS.**
- Flag-absent identity: the C++ change sits inside `if (calibrate.on())`, and Python mode's verify receipts keep their keys (case E).
- M1c-RED: the diff has no `atx-engine` file; `stats_ext.hpp` and `book_normal_score_test.cpp` are untouched, and `BookNormalScore.*` is green in Debug and Release.
- No expected value edited: the only changed assertion is the round-1 `construction_reused == (c == 0)`, extended to `c == 0 || warm` for the new case.
- Nothing dated >= 2024-01-01 was opened, and no 2020-2023 output was opened. All runs here used synthetic fixtures.
- Note: an engine-mode state dir verified under pre-round-2 code would now be STALE at verify, because a new input key was added. None exists; the lane ran no real wave.

## Findings

`path:line | severity | problem | concrete fix`

- **S14** `scripts/wave_stage_record.py:202-228` (`record`) | Suggested.
  - Problem: the R4 pin is checked when verify starts and on every resume. A committed edit of `<cell>-gm.json` made *during* the same run, after verify's check (judge's summ, bundle and book run for minutes in real waves), still reaches `wave_result.build` with match's digest (evidence 2, case F).
  - Fix: just before `wave_result.build`, add `mt = done.get("match") or {}` and then `if mt.get("record_spec") and w.spec_digest(mt["record_spec"]) != mt.get("record_spec_digest"): raise StageError(..., EXIT_PIN)`. This is engine-only, adds no receipt key, and Python-mode bytes do not change. Optionally apply the same check to `cell_spec` / `spec_digest` for Python mode's smaller window.
  - Root can place it at the E2 merge, in the same file and outside E2's hunks.
- Carried and ruled, not counted:
  - S10 (two-speed exclusion untested) and the `--max-bytes` budget-branch test: Ruling C2-8.
  - Default `gross_match` after the Python rule is deleted: Ruling C2-9, root or C3.

## What root must check at merge (round 1 list, updated)

1. **Builds.** Build from a clean tree (DirtyEntries 0) at the merge SHA, equity-dev and equity-rel. The lane's k and l receipts carry 1 dirty sprint-doc entry and are not merge evidence. Targets:
   - `atx-impl-strategy-target-tests`
   - `atx-equity-strategy-targets`
   - `atx-engine-eval-tests`
   - `atx-engine-book-tests`
   - `atx-impl-tests` (its glob compiles `strategy_nav_calibrate_test.cpp`; never built by the lane)

   gtests:
   - `CalibrateGross.*:NavBookRule.*:BookNormalScore.*`, then the whole target-tests binary
   - `atx-impl-tests --gtest_filter=CalibrateGross.*`
   - eval `*StatsExt*:*Deflated*:*MinTrl*`
   - book `BookNormalScore.*:*DecayMonitor*`

   Both `BookNormalScore.*` must be green in Release.
2. **ccache stale-PCH trap** (ledger candidate 4). If `deps/<preset>` was fetched again since the tree's last PCH build, the restored PCH refuses every consumer. Delete by name `cmake_pch.cxx.{pch,obj}` for atx-engine and atx-impl-core, then rebuild with `CCACHE_RECACHE=1`.
3. **X-5 gm-pair identity run.**
   - Run `nav <X-5 argv at L> --calibrate-gross <repr(G_parent)>` and compare it with the two-process `-gm` NAV. Every file must be byte-identical except summary.json `/calibration` and `/producer`. Calibration `leverage` must equal L', and `gross` must equal the `-gm` NAV's mechanics G.
   - Read `calibration.construction_reused`. X-5 carries `--warm-start-sessions 60` and `--max-bytes 1 GiB`, so this is the real-data cover of R5 and S9.
   - With S9 the tape's full bound is reserved up front, so record the peak RSS against the budget, and the wall time before and after.
   - Then compare the engine wave with the Python wave on leverage, gross, gross_parent, gross_calibration, leverage_calibration, corrected and the NAV bytes. Engine `cell.spec` must be the plain `-gm` at L', with `run_spec` = `-cal`.
4. **Python rule deletion.** Only after item 3, delete `wave_rules.py:110-119` at base (`:113-122` at the lane head), with its Python `match` path and `FakeCycle.calibrate` callers. Implement Ruling C2-9's plan-time refusal (a manifest that needs gross matching without `gross_match`) in the same commit, or C3 carries it.
5. **No `rel` references.** No P9 reference may have been produced under the `rel` preset; references are equity-dev and equity-rel only (C2-5 for rel-avx2).
6. **Keep-both merges with E2 `d0054b32`.**
   - `wave_stage_record.py` (R4's two lines, accepted cross-lane edit C2-8), `wave_stage_cell.py`, `wave_result.py` and `wave_steps.py` merge with 0 conflicts (verified).
   - Apply S13 exactly as written above, and S14 if taken.
   - COV shares the `atx-impl/tests/CMakeLists.txt` tail.
7. **Ledger:** candidates 1-4 in the report (2 as amended, 4 = the PCH trap).
8. **AL-CLOCK (wave 3b):** if its per-name rate reads the leverage inside the shared construction, it joins `construction_reusable`'s exclusions (`strategy_nav_replay.cpp:3496`) with a test.

## Checked

- [x] `.agents/cpp/agent.md` §10 on the C++ diff:
  - bounds: the reserve is bounded by the admitted budget, and there is no overflow;
  - lifetime: `tape` is in function scope;
  - error path: a failed reserve is caught before publish;
  - 0 added lines over 100 columns;
  - /W4 /WX clean per the k and l logs.
- [x] Diff inside scope plus the listed cross-lane edit (`wave_stage_record.py`, E2's, accepted by C2-8).
- [x] Report evidence matches the claims where re-runnable: pytest 20; the R4 test's paths; gtest logs 19/19 and 348/348 in Debug and Release; receipts.
