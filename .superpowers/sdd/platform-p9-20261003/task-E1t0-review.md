# Lane E1 task 0 (P0-FIX) review

## Verdict
APPROVE

Spec compliance: ✅ (brief-E1 task 0 (a)-(g); plan §1.2 "before (P0-FIX)" rows; DEC-1, DEC-2, DEC-4; §0.6; rulings P1, P3)

## Reviewed SHA
`3fa2dd4affe4b4de718d222435801b318ebf5cf0` (parent `d7c1c520`, one commit, 18 files; report commit `a57961bb` is
docs only). Root merges exactly 3fa2dd4a.

## Evidence
Pool-17 at HEAD a57961bb (code identical to 3fa2dd4a: `git diff --quiet 3fa2dd4a HEAD -- scripts atx-impl`), tree
clean before and after, and no `.pyc` newer than the session start.

1. `PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   scripts/tests/test_wave_hardening.py scripts/tests/test_research_spec.py scripts/tests/test_research_mine.py
   scripts/tests/test_wave_speed.py scripts/tests/test_research_cycle.py` -> exit_code=0
   ```
   .............s...........s.............................................. [ 95%]
   ..........                                                               [100%]
   223 passed, 3 skipped in 247.27s (0:04:07)
   ```
2. `PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=1 ... -m pytest -q -p no:cacheprovider scripts/tests/test_wave_hardening.py
   atx-impl/tools/test_backtest_integrity.py atx-impl/tools/test_trial_ledger_rules.py atx-impl/tools/test_nav_summ_v8.py
   atx-impl/tools/test_dsr_total.py` -> exit_code=0
   ```
   ...........................s.................                            [100%]
   44 passed, 1 skipped in 13.83s
   ```
3. In memory, no file written: `wave_manifest.validate` on the committed `y-s.json` returns only the
   `marginal.seconds 720 is above the bounded runner's maximum 600 s` refusal. With root's two amendments
   (`seconds` 600; `admission_cycle_prefixes ["v8x","v8ys"]`) it returns `[]`. With both prefix keys present it refuses.
4. Scan of all 72 committed `scripts/specs/**` JSON files: the only time caps above 600 are `y-s.json` and
   `y-s.head.json` `marginal.seconds 720`, which root amends. No other spec or template stops loading.
5. Capacity completeness was checked by file names only in root's `build-equity`, and no NAV content was opened.
   - Receipts: every NAV run receipt whose `command` carries `--capacity-curve` was checked (receipts are open). 41 are
     completed, and all 41 output dirs hold `summary.json`, `capacity_curve.csv` and `v7_extras.json`: 0 incomplete.
   - Directories: every dir with `summary.json` plus a `capacity/` subdir was checked. There are 41, with 0 incomplete.
   - X-5's NAV dir `mega-nav-v8x-theme-erc-L1.1720` holds all three files.
   - C++ at d7c1c520 agrees. `strategy_nav_v7.cpp` dispatches `nav` to v7 whenever `--capacity-curve` is in the argv.
     A successful run writes `summary.json`, then the capacity pass, then `capacity_curve.csv` and `v7_extras.json`.
     A void run writes no summary. `summary.v7.declarations.capacity_curve` is the key that `capacity_expected` reads.
     The capacity CSV `book` column is `scenario.id + "+" + financing.id`, which equals the daily file's scenario
     name, so the 4x row is unique.
6. Does the OR-2 logic change the registered Y program? No:
   - **Fields differ, so the ref already runs.** The Y-S cell's `baseline_fields` is X-5's fields (v13, from
     `research_add_alpha.py:168` via the `lib-v8x3b-gm.json` chain), and the cell runs on v15. The ref step
     therefore runs exactly as before, and the new forced-ref branch (`research_cycle.py:1035`) is a no-op for Y-S.
   - **Exes differ, but verify does not refuse.** X-5's NAV receipt has `executable_sha256` a95f6f0a…, and root's
     current NAV exe is 72ff6d2d…, so verify records `equal: false`. Verify still records `ref: "ran"`, not a
     refusal: the ref and nav run in the same calibration `run --stop-after nav`, and `gm_doc` keeps the ref output.
   - **Rule-cell templates are untouched.** The rule-cell templates (Y-3, Y-2, Y-5, Y-1 and X-10 as a template)
     lose `ref` and `baseline_fields` at resolution (`research_spec` IDENTITY_SECTIONS / IDENTITY_INPUTS), so neither
     branch reaches them.
   - **No other wave manifest exists.** The only committed wave manifests are `y-s.json` and `y-s.head.json`.
7. DEC-4 (byte neutrality) holds:
   - No exe argv changes, and no Python numeric path changes.
   - `spec_digest` hashes spec files only, so every existing `cycle_binding.json` still matches.
   - The only `script_sha256` recorded in outputs is that of `nav_summ.py` or the fitter, and neither file changed.
   - Budget output for the string form is byte-identical: the `**prefix` key sits in the same position, and
     `budget_line` prints the same "v8x*".

## Findings
atx-impl/tools/backtest_integrity.py:1148 | minor | the `finally: lock.unlink(missing_ok=True)` call has no Windows sharing-violation handling. While another handle is open on the lock, the unlink raises PermissionError after the lines were already appended. That handle can be a waiter reading the holder at its deadline (`:1113`, an open without FILE_SHARE_DELETE) or an AV/indexer. The error masks the successful return, so the summ step HARD-STOPs with the ledger already written, and it leaves a stale lock. The window needs about 60 s of contention, and root runs one process at a time, so this is not a merge blocker | in wave-1 proper, retry the unlink briefly on PermissionError, and warn rather than raise when the body succeeded. Report the holder without opening the lock (os.stat), or tolerate the read failing.
atx-impl/tools/backtest_integrity.py:1110 | minor | a stale lock and a permission error both look like a live holder. A writer killed by the bounded runner mid-append leaves `<ledger>.lock` behind, and every later append then waits 60 s and refuses. A persistent PermissionError, such as an unwritable dir, waits 60 s and then reports "held (unreadable)". This fails closed and is listed in the lane's open risks | in wave-1 proper, on timeout say whether the recorded pid is alive, and name the permission case separately.
scripts/cycle_resume.py:43 | minor | there are two enumerations of "the parent NAV's last completed exe". research_cycle scans `-run` and `-run2..-run9` (MAX_ATTEMPTS), while verify (`wave_stage_record.py:70` via `Wave.run_dirs`) scans `-run` and any `-run<k>`, including `-run1` and `-run10+`. If the two disagree, verify's added refusal (`wave_stage_record.py:42`) fires where research_cycle kept the skip. This cannot arise for Y-S: research_cycle never writes `-run1`, and attempts are at most 9 | in wave-1 proper, use one helper for both, e.g. verify calls `cycle_resume.completed_exe_sha256` with the same attempt bound.
scripts/tests/test_research_spec.py:366 | minor | the line that replaced the registry assertion is a tautology. V8_SPECS excludes GENERATED by construction, so `spec_kind(...) in (BASE, TEMPLATE, ADD_ALPHA_COPY)` can never fail. The real check is sound: `check_nominal_plan` against `spec_null_pins`, anchored by the seven literal sets in `test_spec_kind_null_pins` | drop the line, or assert something falsifiable, e.g. that a `lib-*-gm.json` copy whose base is generated is ADD_ALPHA_COPY.

## Checked
- [x] .agents/cpp/agent.md §10: N/A (no C++ in the diff); house Python rules hold. Iteration is deterministic: `run_dirs` is sorted, and the prefixes keep manifest order. No seal or look-ahead path is touched, and no Python copy of a C++ numeric rule was added (`CAPACITY_FILES` is a file contract).
- [x] Diff stays inside the brief's files-in-scope. Task-0-only files are `research_cycle.py` (P3), `backtest_integrity.py` (`ledger_append` only) and `test_research_spec.py`. There are no `scripts/specs/**`, C++ or `atx-db/` files. One declared cross-lane edit, `scripts/tests/test_research_mine.py:131-135`: no wave-1 brief owns it, and it was needed because the runner literal it grepped changed. It is equally strong: it asserts that `research_tree.RUNNER_MAX_SECONDS == research_mine.RUNNER_MAX_SECONDS` and that the runner line uses the constant.
- [x] Evidence in the report matches its claims; spot checks were re-run (above).
- [x] Lane items verified against the code:
  - **(a)** The 600 cap is enforced at four points: manifest load, spec load, the wave step, and the runner itself.
    - Manifest load: `wave_manifest.py:241`.
    - Spec load: `runner.seconds` and every `runner.phases.<p>.seconds`, via `validate_runner_phases`, exit 2.
    - Wave step: `marginal_ruled`.
    - Runner: `run_bounded_research.py:92`, where the message `<=600 seconds` is unchanged.
  - **(b)** Exactly one of the string and list prefix keys may be present, and both pair with `admission_cap`. The list
    must be non-empty, distinct texts. The only consumers (`wave_stage_preflight`, `wave_result`) handle both forms.
  - **(c)** For the nav and ref steps, an argv with `--capacity-curve` is done only when all three files exist; a
    summary without the other two is failed and never overwritten. The book reader refuses the missing file, or a
    missing or non-single 4x row, when the summary declares the curve.
  - **(d)** Phase rows and wave-result timings carry `executable_sha256`. Verify records `nav_exe`. The ref is forced
    only when fields are equal and both exe SHAs are known and differ; an unknown on either side keeps today's skip.
  - **(e)** O_CREAT|O_EXCL lock. Lock fds are non-inheritable (PEP 446). The lock is removed in `finally` on a
    refusal or exception. The appended bytes are pinned exactly, and the test runs 12 concurrent writers.
  - **(f)** The kind rule anchors seven specs to literal sets, and a new template and a new -gm copy both plan with no
    edit to the test file. A wave-written gm copy (`wave_steps.gm_doc`: name + "-gm", the add-alpha description kept)
    classifies as an add-alpha copy.
  - **(g)** `run_two_seeds.py` is not collected by pytest. It runs both seeds and exits with the first failure code.

## Notes for root / PM (no fix required before merge)
- **Root's pre-R0-6 concern (1) does not apply to the skip logic.** Y-S's ref runs anyway because its fields differ.
  It must still reproduce X-F0's S2 daily, which P8 already showed (R0-4).
- **Root's concern (2) is answered:** X-5's NAV dir and all 41 capacity NAV dirs are complete by file name.
- **The book reader's refusal also covers a non-finite 4x `net_sharpe`.** The C++ writes `nan` through
  `finite_or_null`. That is in-spec ("refuses None"), and R0-6's P12 check on one real `capacity_curve.csv` covers it.
- **Deviation (d): verify also refuses** when the exes differ and no completed ref ran on the cell's exe. It can only
  fire if the NAV exe is rebuilt mid-wave (between the calibration run and the matched run), and then it is the
  correct stop. The PM may want to record it as a ruling.
- **`--runner-override seconds=N` is still unchecked at load.** The runner refuses it with exit 2, which is a "no
  receipt" HARD-STOP, so it was outside the named lines and is left for task 1+.
