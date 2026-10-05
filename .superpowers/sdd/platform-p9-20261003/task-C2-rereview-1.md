# Lane C2 re-review 1 (fix round 1: `nav --calibrate-gross`, `gross_match: pm6-6-engine`, M1c-RED)

## Verdict

**CHANGES_REQUIRED.** There are 2 Required items and 6 Suggested.

| Item | Result |
|---|---|
| R1 | closed |
| R2 / C2-R2 | closed as written, but the cell of record is not pinned after the match stage (**R4**) |
| R3 / C2-R3 | closed, bit for bit against numpy 1.26.4 |
| C2-4b, C2-6, C2-7 | closed |
| M1c-RED | unchanged: comments only, both tests green in Debug and Release |
| Cross-lane edit `wave_result.py` | minimal and listed; merges cleanly with E2's `05a0fcd1` |
| S2 | partial. Warm start is reachable by every X-5-lineage cell and is untested under reuse (**R5**) |

## Reviewed SHA

`e68adf41` on `feat/p9-c2-20261003` (pool-15). Lane base `ad406718`. The previous review `4545cec4` was at `08297183`.

Fix-round commits:
- `c9024beb`: the C++, committed unbuilt.
- `3eca79bf`: the hand-off.
- `e68adf41`: the Python, the report and the logs.

`git diff c9024beb e68adf41 -- atx-engine atx-impl CMakeLists.txt cmake` is empty, so the C++ the lane built is the head's. HEAD is unchanged, and the tree was clean before and after (the only ignored `__pycache__` in pool-15 dates from 2026-10-02 and is not mine).

## Evidence (run by the reviewer)

1. **Lane pytest re-run.** Command, from cwd pool-15:
   ```
   PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider --basetemp <scratch>/c2rr1-bt C:/atx-wt/pool-15/scripts/tests/test_research_wave.py
   ```
   Output `19 passed in 24.63s`, exit 0.

2. **numpy 1.26.4, bit for bit** (scratch `c2rr1_numpy.py`, synthetic only, Python 3.12.2 / numpy 1.26.4). Two models were compared against `float(np.array(a, float64).mean())`:
   - My own derivation from `loops_utils.h.src` `DOUBLE_pairwise_sum`:
     - n < 8: 0.0 then sequential adds;
     - n <= 128: 8 accumulators seeded with a[0..7], a step of 8 up to n - n%8, then `((r0+r1)+(r2+r3))+((r4+r5)+(r6+r7))`, then the tail;
     - above 128: split at n/2 - (n/2)%8;
     - the reduce adds this to the identity 0.0 once per 8192-value buffer chunk.
   - A line-by-line transcription of the head's C++ `pairwise_sum` / `numpy_mean`.

   Data: 5 series (the lane's ramp and hash, uniform gross-like, lognormal sigma 6, sign-alternating lognormal). Lengths: 374 sizes, namely 1..299 plus 128/256/1024/4096/8192/16384/24576/32768/65536 each ±1/±7/±8/±9, and 1000, 1023, 2517, 2600, 3001, 9000, 12345, 20001, 40000, 40003, 100001, 131071, 250007. That gives 1,861 cases.

   Results:
   - My model vs numpy: **0 mismatches**.
   - C++ transcription vs numpy: **0**.
   - C++ vs model: **0**.
   - Wrong models do miss: unblocked pairwise 100 (first at ramp 8193); buffer 4096: 95; buffer 16384: 64; first-element initial: 767 (first at ramp 17); naive sum: 1,286.
   - All 20 fixture bit patterns of `CalibrateGross.NumpyMeanIsNumpysMeanAtEverySize` equal numpy and the transcription.
   - The fixture has teeth: it fails the unblocked (ramp8193, hash40000), buffer-4096 (hash16384/16385/20001), buffer-16384 (ramp8193), first-element-initial (6 rows) and naive (16 rows) models.

3. **Window demonstration for R4** (scratch `c2rr1_window_test.py` on the lane's `wave_fixture`, synthetic). The setup was `--until match`, then a committed edit of `lib-w1b-gm.json` `nav.leverage` to `1.9999`, then `run`:
   - **Engine mode** exits 0. wave-result `cell.spec` is `lib-w1b-gm.json`, and its `cell.spec_digest` no longer equals the file's digest. `cell.leverage` says `1.1782`, the file says `1.9999`, and `next_parent.spec` points at the edited file.
   - **Python mode** (control) exits 3: `STALE: match recorded spec_digest 38d6a33c...; it is 4264ad4c... now`.
   - `2 passed in 6.78s`. Scratch deleted.

4. **Merge with E2's head** (`git merge-file`, common base `1239a5ff`, which equals `ad406718` for these files):
   - `scripts/wave_result.py`: 0 conflicts. The merged file is E2's file plus exactly C2's two hunks.
   - `wave_stage_cell.py`: 0 conflicts. `wave_fixture.py`, `test_research_wave.py`, `wave_rules.py`, `wave_steps.py`: 0 conflicts.
   - E2 does not touch `wave_stage_record.py:verify_inputs`, which is where R4's fix goes.

5. **Logs and receipts read.**
   - `c2-fix1-gtest-{h,j}-named.log`: `CalibrateGross.*:NavBookRule.*:BookNormalScore.*` 19/19 in both Debug (h) and Release (j), including `BookNormalScore.TiesShareTheMeanRankAndMirrorsAreOpposite`, `.FixtureTellsWrongRulesApart` and `.NormPpfOfALiteralIsItsRunTimeValue`.
   - The `-whole` logs show 348/348 for both.
   - Receipts `mega-p9-c2-{h,j}-receipt.json`: Source `3eca79bf`, exit 0. **DirtyEntries 2 / 5, not 0.** The receipts do not name these entries. The C++ at Source equals the head's, so this is a root check (below) and not a finding.

6. **Column check.** 0 added C++ / CMake lines over 100 columns and 0 Python lines over 120. 40 templates in `scripts/specs` set no `ref` of their own (scratch scan).

## Item by item

**(a) R1: closed.**
- Plain spec: `calibrated_doc` (`wave_stage_cell.py:313-317`) sets `ref.flags` to the cell's own pre-calibrate `nav.flags` and `ref.leverage` to L, using `setdefault`.
- Template branch (`:304-309`): `change.set["ref.flags"/"ref.leverage"]` is appended after the template's own `ref` key, from the resolved spec. `resolve` drops a parent's ref (`research_spec.py:53,177`), so only a template-set ref is pinned, and a ref that sets its own flags or leverage keeps them.
- Only `ref_step` and `nav_step` read `nav` (`research_cycle.py:1186,1210`). The `{leverage}` substitution gives the same argv as the uncalibrated cell, so the ref binding (argv-only, `cycle_resume.py:185`) matches.
- Rule cells have no ref. On resume, the write-once spec writer refuses a changed `-cal`.
- Test teeth: the round-0 recipe fails `ref_nav(cal) == ref_nav(doc)`. S11 covers the test's local copy of the merge.

**(b) R2 / C2-R2: closed as specified.**
- `record_doc` (`:321-331`) is `gm_doc` at L', with the NAV output set to the engine dir. It has no `--calibrate-*`, and its ref uses L and the same argv.
- `wave_result.cell.spec` and `next_parent` point at the record. A template child resolves at L' with the Python mode's flags, as tested. An add-alpha child pops the ref and copies nav (`research_add_alpha.py:206-208`), so it also starts at L'.
- **However**, after match nobody re-pins `record_spec_digest`:
  - `verify_inputs` pins `mt["cell_spec"]`, which is the `-cal`.
  - `judge_inputs` and `record_inputs` pin verdict and bundle files.
  - `require_clean` only refuses *uncommitted* edits.
- So a committed edit of the cell of record between match and record passes silently. Evidence 3 shows it, and Python mode refuses the same edit. → **R4**.

**(c) R3 / C2-R3: closed.**
- Evidence 2. The C++ structure matches numpy line for line: the `n < 8` tail from 0.0, `n <= 128`, the split rounded to a multiple of 8, and per-8192-chunk `+=` from 0.0.
- `nav_summ.load_daily_csv` builds contiguous float64 (`np.array([float(x) ...])`, `nav_summ.py:216`), which is the contiguous case the model covers.

**(d) C2-4b: closed.**
- The run stage records `spec_digest`, `w_combined` (`Cycle.ic_attempt`, the attempt the NAV step reads) and `w_combined_sha256`, and refuses an incomplete w.
- `match_inputs` (`:424-430`) pins all three. A changed w gives exit 3 (`test_engine_match_pins_the_w_output_its_nav_reads`). Python mode is unchanged.
- The `gm_doc` template branch not pinning a template-set ref is **Suggested** (S8). Reasons:
  - No template sets one (0 of 40).
  - Children never inherit a ref: `resolve` drops it, and add-alpha rebuilds it.
  - The published book (the NAV) and next_parent's nav are unaffected.
  - Python mode has the same latent defect.
  - Re-running the record itself is refused by the binding anyway (C2-6).

**(e) M1c-RED: unchanged.**
- This round touched `stats_ext.hpp` in comment lines only (S3). `book_normal_score_test.cpp` was not touched.
- No expected value was edited:
  - The numpy fixture keeps round 0's three ramp pins (5 / 129 / 1023) at the same bits.
  - It drops n = 100 / 1000, where the old and new algorithms agree.
  - It adds 17 numpy-printed pins.
- Both `BookNormalScore.*` reds are green in Debug and Release (evidence 5).

**(f) Cross-lane `wave_result.py`: PASS.**
- The change is 3 doc lines plus 2 changed and 2 added in `build()`. It is listed in the report.
- Python mode is byte-identical: no `record_spec` means the same `spec` / `spec_digest` and no new key (tested).
- It merges cleanly (evidence 4).

**(g) S1-S7:**
- S1, S3, S4, S7 are fixed as claimed.
- S5 and S6 are reworded (root items 2 and 3).
- On S2's three uncovered cases:

| Case | Reachable by a P9 cell? | Outcome |
|---|---|---|
| Warm start | **Yes.** `--warm-start-sessions` is in 17 `scripts/specs/v8` specs, including `lib-v80` / X-5. The lane's reason, "the fixture's score_begin is 0", does not hold: `artifact_from(dir, p, begin)` (`strategy_nav_replay_test.cpp:2818`) and the `NavWarmStart.*` suite sit in the same file. | **R5** |
| `--max-bytes` budget | **Yes** (X-5's 1 GiB). But a false predicate selects `tape = nullptr`, which is exactly the path the adv-hold and hold-band cases already pin byte for byte. A true predicate can only cost memory: a failed allocation is caught and refused. No predicate outcome can change a published byte. | Suggested (S9). PM may overrule. |
| Two-speed | **Not by a planned cell.** Only the Y-5 lineage sets `--two-speed`. Y-5 was NOT ACCEPTED (ledger R0-9), so it is no parent. No P9 template or queue entry sets the flag, and AL-CLOCK is a trade-rate registry rule, not `two_speed`. The exclusion reads the same `target.two_speed` that the lockstep grouping reads (`strategy_nav_replay.cpp:2444`). | Suggested (S10) |

**(h) Flag-absent identity: PASS.**
- C++: `validate_config` compares against `aim_leverage_min/max` (1.0 / 2.0), the same values as before. `numpy_mean` sits only on the calibrate path. The rest of the round's C++ changes are comments and line wraps.
- Python: the mode branch adds no key to the run, match or wave-result outputs, and `FakeCycle` changes only on `--stop-after w`.

**(h) No look-ahead, no data, no statistics.**
- No window logic changed.
- Nothing dated >= 2024-01-01 was opened, and no 2020-2023 output file was opened.
- Disclosure: while checking whether two-speed is reachable, I printed ledger line `progress.md:147` (R0-9). It carries Y-5's published verdict statistics, which are ledger-public. No output file was read.

## Findings

`path:line | severity | problem | concrete fix`

### Required

**R4** `scripts/wave_stage_record.py:256-263` (`verify_inputs`), with `scripts/wave_stage_cell.py:396` | **Required (major)**

Problem:
- In engine mode the match stage hands on the cell of record (`record_spec`, `record_spec_digest`), but no later stage pins it. `verify_inputs` re-hashes only `mt["cell_spec"]`, which is the `-cal` recipe.
- A committed edit of `<cell>-gm.json` after match therefore runs to exit 0. wave-result then records a stale `cell.spec_digest`, and `next_parent.spec` points at a recipe that does not reproduce the published NAV (evidence 3).
- Python mode refuses the same edit (exit 3), because its cell_spec *is* the `-gm`.

Fix:
- In `verify_inputs`, add `if mt.get("record_spec"): now["record_spec_digest"] = w.spec_digest(mt["record_spec"]) if w.exists(mt["record_spec"]) else None`. `pinned("match", mt, now)` then compares it with the match receipt before verify and on every resume.
- This is a named cross-lane edit in E2's file, outside E2's hunks, so it merges cleanly.
- Add a pytest: the evidence-3 sequence, expecting exit 3 `STALE: match recorded record_spec_digest`.
- Optionally, have `wave_result.build` refuse when `w.spec_digest(cell["spec"]) != cell["spec_digest"]`.

**R5** `atx-impl/tests/strategy_nav_replay_test.cpp:3527` (`ReuseAndItsExclusionsPublishThePlainRunAtTheFinalLeverage`) | **Required (test gap)**

Problem:
- Construction reuse under a warm start is untested. Pass 1 records decisions from `begin - K`, `decisions = ... + warm_start_sessions` (`strategy_nav_replay.cpp:3493`), and pass 2 restores them. Every X-5-lineage engine cell (`--warm-start-sessions 60`) takes this path.
- The stated blocker does not exist: the warm-start fixture is in the same file.

Fix:
- Add a fourth case: the X-5 keys plus `a = artifact_from(dir.path, p, K)` and `execution.warm_start_sessions = K`. Pick K small enough that S2 is not flat at `score_begin`, or `refuse_inert_warm_start` fires.
- Assert `construction_reused` true, every file byte-identical to the plain run at L' (summary.json without `calibration`), and `corrected` true.
- If the fixture cannot build a book after the warm-up, refuse `--calibrate-gross` with `--warm-start-sessions` instead (the dispatch's alternative).

### Suggested

**S8** `scripts/wave_stage_cell.py:327-328` (via `wave_steps.py:157-160`) | Suggested
- Problem: `record_doc`'s template branch inherits `gm_doc`'s gap. A template that sets its own `ref` without `leverage` gets a record whose ref resolves at L'. The `-cal` recipe pins that ref at L. No template does this today, and Python mode shares the gap.
- Fix: in `record_doc`'s template branch, set `change.set["ref.leverage"]` to the resolved L when the resolved ref lacks it, mirroring `calibrated_doc:304-309`. Alternatively, refuse a template-set ref in engine mode.

**S9** `atx-impl/src/strategy_nav_replay.cpp:1306-1320, 3493-3495` | Suggested
- Problem: `construction_tape_bytes` bounds the tape's *size*, but `record_construction` grows three vectors with `insert` and no `reserve`. Capacity can exceed the bound by the growth factor, plus a transient copy during reallocation, so `--max-bytes` is not a hard bound on the tape.
- Fix: in record mode, `reserve` the vectors at the bound once, or count capacity. A gtest of the budget branch is optional: its outcome path is already pinned.

**S10** `atx-impl/src/strategy_nav_replay.cpp:3486` | Suggested
- Problem: the two-speed reuse exclusion is untested.
- Fix: add a sleeve-artifact case, or refuse `--calibrate-gross` with `--two-speed`, before any two-speed cell is queued for engine mode.

**S11** `scripts/tests/test_research_wave.py:300-303` | Suggested
- Problem: `ref_nav` is a test-side copy of `research_cycle.Cycle.ref_step`'s merge, so it can drift.
- Fix: build the step through `RC.Cycle(...).steps()` / `ref_step` and compare the ref step's argv.

**S12** `.superpowers/sdd/platform-p9-20261003/task-C2-report.md:237` | Suggested
- Problem: ledger candidate 2 says "a first-element-initial reduction differs at any n". It *can* differ from small n on; the first miss here is ramp n = 17.
- Fix: say that.

**S13** `scripts/wave_stage_cell.py:76` | Suggested (merge)
- Problem: E2 rewords the Python branch's exe-notes log line (`:86` here). The engine branch keeps the old wording.
- Fix: align the wording at the keep-both merge.

## What root must check at merge

1. **Build from a clean tree** (DirtyEntries 0) at the merge SHA, on equity-dev and equity-rel. Targets:
   - `atx-impl-strategy-target-tests` and `atx-equity-strategy-targets`
   - `atx-engine-eval-tests` and `atx-engine-book-tests`
   - `atx-impl-tests`: its glob compiles `strategy_nav_calibrate_test.cpp`, and the lane never built it.

   gtests:
   - `atx-impl-strategy-target-tests --gtest_filter=CalibrateGross.*:NavBookRule.*:BookNormalScore.*`, then the whole binary.
   - `atx-impl-tests --gtest_filter=CalibrateGross.*`
   - `atx-engine-eval-tests --gtest_filter=*StatsExt*:*Deflated*:*MinTrl*`
   - `atx-engine-book-tests --gtest_filter=BookNormalScore.*:*DecayMonitor*`

   Both `BookNormalScore.*` reds must be green in Release (M1c-RED lifted). The lane's receipts carry 2 / 5 unnamed dirty entries, so they are not merge evidence.

2. **ccache stale-PCH trap (p9-c2-i).**
   - Cause: a ccache-restored clang PCH embeds the include mtimes of the `deps/<preset>` fetch that built it. After `deps/<preset>` is deleted and fetched again, every PCH consumer is refused ("... has been modified since the precompiled header was built").
   - Fix: delete by name the two `cmake_pch.cxx.{pch,obj}` (atx-engine, atx-impl-core), then rebuild with `CCACHE_RECACHE=1`.
   - Ledger the trap.

3. **X-5 gm-pair identity run.**
   - Run `nav <X-5 cell argv at L> --calibrate-gross <repr(G_parent)>` and compare it with the two-process `-gm` NAV. Every file must be byte-identical except summary.json `/calibration` (added) and `/producer`.
   - `calibration.corrected` / `.leverage` (L') / `.gross` must equal the `-gm` NAV's mechanics G.
   - Read `calibration.construction_reused` (S6 / S9). X-5 carries `--warm-start-sessions 60` and `--max-bytes 1 GiB`, so this run is also the one-off real-data cover of R5. Record the wall time before and after.
   - Then the engine wave against the Python wave, compared on leverage, gross, gross_parent, gross_calibration, leverage_calibration, corrected and the NAV bytes.
   - Engine `cell.spec` must be the plain `-gm` at L' and `run_spec` the `-cal`. `nav`, the dir name (C2-7), `spec_digest`, `commit` and `readers` differ by design.

4. **Only after item 3:** delete `wave_rules.py:110-119` at base (`matched_leverage`, `gross_matches`; `:113-122` at the lane head), per G-P5. Its callers go with it: the Python `match` path and `FakeCycle.calibrate`. P15 makes the Python `pm6-6` the default when the manifest key is absent, so that slice needs a PM ruling on the default (absent → engine, or absent refused).

5. **No P9 reference was ever produced under the `rel` preset** (IPO on; constant-argument `norm_ppf` folds can move, as on rel-avx2 per C2-5). References are equity-dev and equity-rel only.

6. **Keep-both merges.**
   - `wave_result.py` and `wave_stage_cell.py` merge cleanly with E2 `05a0fcd1` (verified).
   - R4's `verify_inputs` line goes into E2's `wave_stage_record.py`, outside E2's hunks.
   - Apply S13's wording.
   - COV shares the `atx-impl/tests/CMakeLists.txt` tail.

7. **Ledger:** candidates 1, 2 (with S12's wording) and 3, plus the PCH trap.

8. **AL-CLOCK (wave 3b):** if its per-name rate reads the leverage inside the shared construction, it must join `construction_reusable`'s exclusions (`strategy_nav_replay.cpp:3486`) with a test.

## Checked

- [x] `.agents/cpp/agent.md` §10 on the C++ diff: no UB, narrowing, lifetime or bounds issue; `std::min` on two `usize` values; recursion bounded (`log2(n/128)+1`); /W4 /WX clean per receipts; 100 columns met.
- [x] Diff inside scope plus listed cross-lane edits (`wave_result.py`, E2's). No COV-owned file touched.
- [x] Report evidence matches the claims where re-runnable: pytest 19; numpy fixture and model; gtest logs 19/19 and 348/348 in Debug and Release; receipts (dirty entries noted).
