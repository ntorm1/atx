# Lane E1 review (tasks 1-7, wave driver hardening)

## Verdict
BLOCK (1 major, 7 minor). The major is in an opt-in path (`--host-budget-mib` / `driver.host_budget_mib`). Flag-absent
identity holds, and resume / attempt / ledger safety holds. Fixing the major and re-reviewing is enough to approve.

## Reviewed SHA
`e40c9152` (code head `d19606dd`, tasks 1-7 = e93d5c2b..d19606dd), base `3fa2dd4a` (task 0, not re-reviewed).
Diff read per file: `git -C C:/atx-wt/pool-17 diff 3fa2dd4a e40c9152` (26 files, +2542/-243).

## Evidence
1. `scripts/tests` in pool-17 at e40c9152, clean tree:
   `PYTHONHASHSEED=0 PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests`
   gave exit 0:
   ```
   ......................................................                   [100%]
   338 passed, 4 skipped in 261.55s (0:04:21)
   EXIT=0
   ```
   Afterwards `git status --short` was empty and HEAD was still e40c9152.
2. Flag-absent identity, re-run against the review base `3fa2dd4a`. The lane compared against e93d5c2b, which leaves
   task 1 out of its check. Method: `git archive` of scripts/, atx-engine/tools, atx-impl/tools and atx-impl/strategies
   at both SHAs into the scratchpad; the `wave_fixture` world (sign rule drops alpha_b, calibration + matched NAV) run
   end to end with `research_wave.main(["run", ...])` and no `driver` key, once plain and once with the PM8-15
   marginal ruling. Result:
   - argv: 22/22 identical (plain) and 23/23 identical (ruled).
   - driver log: 57/57 and 59/59 lines identical.
   - files: 95/95 and 96/96, the same set at both SHAs. After normalising the world path, time keys, git SHAs and the
     receipt digests that hash time, the only remaining differences are those normalised fields (bundle and reader
     receipt binding paths under the world dir, commit short SHAs in wave-log.md, and `wave_result_sha256`).
   - Identity holds.
3. Admission over-commit, reproduced in the scratchpad with `RB.admit` (fake `available`, a shared claims dir,
   budget 12000):
   ```
   u admitted: 3584 need 3072 | ref admitted: 3584 need 2048 claimed_by_others 2560
   joint declared peak + floor = 4608 MiB > free 3584 MiB
   ```
4. A template `lock --exes --write` round trip works in the scratchpad. A child inherits the parent's `exes_sha256`
   and refuses after a rebuild until it is re-locked (see m3).

## Findings
scripts/run_bounded_research.py:220-233 | major | Launch admission can over-commit host memory, which the parallel mode exists to prevent. Each runner checks `free >= its own cap + floor` outside the claims lock (:228). It then claims against `host_budget_mib`, which counts declared caps only and never compares them with free memory. research_cycle starts ref and u (and card and marginal) in two threads at the same instant, and neither child has allocated anything yet, so both runners read the same free memory and both are admitted. With the host's ~3.5 GB free (plan section 0.6) and the v8 caps (u 2560, ref 1536, card 2560, marginal 1536, floor 512), the pair's joint declared need is 4608 MiB against 3584 MiB free (evidence 3). The merge note's `host_budget_mib: 12000` on a 16 GB host never binds. The likely outcome is that both runners floor-kill (system-memory-limit). An attempt that already wrote output is then not auto-retried and needs a manual --suffix. A claim also outlives nothing it should: it is keyed to the runner pid, so a hard-killed runner's orphaned child keeps its memory after the claim is dropped. Tests cover a single admit only. | Under the claims lock, admit only when `free - sum over other live claims of max(0, claim.mib - current tree RSS of the claimant) >= need`. The conservative alternative is `free >= need + sum(other live claims)` whenever a budget is set. Keep the floor check inside the same critical section. Add a test with two admits sharing a claims dir and a constant `available` (the second must wait). Correct the merge note: the budget must not exceed the free memory at wave start.
scripts/run_bounded_research.py:167-183 | minor | Several HostClaims lock failures end as `runner-error`, which is not in AUTO_OUTCOMES, so an admission problem becomes a hard stop instead of an `--auto-attempt` retry. (a) `acquire_lock` catches only FileExistsError. On Windows a lock being removed (or held by AV) raises PermissionError, which `ledger_append` handles for this reason. (b) An empty or corrupt lock left by a runner killed between create and write reads as "being written: held" forever, so every admission on the host (the dir is host-wide) waits 30 s and fails. (c) The 30 s lock wait can exceed `--admission-wait-seconds`. | Treat PermissionError like FileExistsError. Reclaim a lock that is unreadable and older than a few seconds (mtime). Bound the lock wait by the remaining admission time and raise AdmissionTimeout, not RuntimeError.
scripts/cycle_resume.py:113-124 | minor | The OR-3 reuse check compares only `command[1:]`; it ignores the receipt's `executable_sha256` (K-P9-10 records it for this). Under `driver.lock_exes` a rebuild makes the Cycle refuse with "a rebuilt exe is re-pinned by `lock --exes --write`" (research_cycle.py:886). Following that advice and resuming then silently reuses u / fit / w / card / marginal outputs made by the old exe while the spec pins the new one. The NAV re-runs only because the spec digest moved. | When the spec carries `exes_sha256`, refuse reuse of a done bounded step whose receipt `executable_sha256` differs from the pin of the exe it ran (opt-in, so identity is kept). Make the PIN MISMATCH message say that outputs made by the old exe must be moved aside or run under a fresh --suffix.
scripts/research_cycle.py:876 (with :2072) | minor | `exes_sha256` sticks beyond the manifest that opted in. A template child inherits its parent's pins (scratch-confirmed), and `research_add_alpha.derive_spec` deep-copies the parent spec, so a b library inherits them too. After one wave runs with `driver.lock_exes`, every later wave without the key refuses (exit 3) at plan or run after any exe rebuild, until someone re-locks by hand. The "absent = before" claim therefore depends on history. This fails closed. | Either strip `exes_sha256` when a spec is derived (template resolve without its own `change.set` pin, add-alpha copy), or have the wave re-lock with `--exes` whenever the parent carries pins. Document the chosen rule in the module doc.
atx-engine/tools/stage_chain.py:93-96 | minor | `content_sha256` drops only the top-level `started_utc` / `seconds`. Stage outputs still carry wall seconds and peaks in their phase rows (run, match, judge, and screen under timings) and per-process seconds (`processes` under driver.timings). The "content" digests of those stages, and the wave-result `receipts` built from them, are therefore still time-dependent, which defeats OR section 3's aim. `test_receipt_digest_time_free` mutates only top-level keys, so it cannot see this. This fails safe (too sensitive, never stale-as-fresh). | Exclude time-valued keys recursively (`seconds`, `peak_mib`, `wall_seconds`, `started_utc` at any depth), or keep timings outside the hashed body. Extend the test to change a nested `seconds` value.
scripts/wave_stage_record.py:162 (with wave_context.py:167, :176; wave_stage_util.py:37) | minor | Under `host_budget_mib`, summ, bundle and book run in three threads, and some records depend on completion order. Console log numbers (`NNN-slug.log`) follow finish order. `w.processes` is appended in finish order, and `fold_processes` keeps first-seen order, so with timings on, the judge receipt's `outputs.processes` order (and its content digest) varies between identical runs. Driver log lines interleave. Failure handling is sound: the pool joins all three, and the first failure in submit order stops the stage. | Sort `fold_processes` output by `what` (or record per-job lists and merge them in submit order). Number consoles in submit order, by reserving the index before the job runs.
scripts/wave_stage_cell.py:73-78 | minor | The code-keyed reuse checks only the `*.py` files the run bound, which are the top-level scripts. The bundle's numbers come from modules nav_summ imports (`backtest_integrity`, `dsr_total`; B2 moves statistics there in wave 2), so a bundle made by old statistics code is reused as fresh. Cycle-level Python phases (card, the fields builder) get no code key at all, although their receipts bind the script. | Bind the imported modules (nav_summ: backtest_integrity.py, dsr_total.py; the readers' helpers) in `bundle_argv` / `reader_argv` so `stale_code` sees them. Note in the module doc that cycle Python phases are not code-keyed, or extend `check_receipt_argv` to compare the receipt's `.py` bindings.
scripts/research_cycle.py:944 (with run_bounded_research.py:271) | minor | The K-P9-10 fields carry little information for every current run. `build_type` is null for all 39 v8 specs (no `build` key) even though their build is the default Debug. `attempt` is 1 for the IC `-run<n>` attempts, the NAV `-run<k>` attempts (`--attempt nav=k`) and the fit's `-run<j>` passes; only OR-4 sub-dirs count. E2 and the scoreboard (the K-P9-10 readers) cannot tell "attempt 1" from "an attempt-k under another numbering". | Pass the runner `--attempt k` for the existing numberings when k >= 2 (k = 1 keeps every current argv). State in the K-P9-10 consumer note that `build_type: null` means "no build key: the default equity (Debug) build".

## Specific checks
- Flag-absent identity:
  - Every new manifest key and CLI flag defaults to absent. `launch` is filtered of Nones, so the header is unchanged.
    `--build-type` is passed only with a `build` key. `unpinned` pops a key that never exists. `verdict_file`,
    `receipt_digest`, `record_date` and `timed` are all no-ops without their key.
  - Iteration order is unchanged without a budget: the judge runs score, verdict, bundle, book as before.
  - The unconditional changes are the ruled or contracted ones: OR-3 refusal (K-P9-10), stale code (E1-STALE),
    `pin --by` roles (plan E1 block), and receipt keys.
  - Re-run against 3fa2dd4a (evidence 2): identity holds.
- Resume safety:
  - OR-3 compares the receipt command with the current step argv.
  - REUSE_NEUTRAL is limited to the screen-u blend switches and the fit's theme-resid input check.
  - Pre-K-P9-10 receipts are reused as before.
  - Attempt-k exists only when the output is absent and the latest receipt is an AUTO outcome. Each later read
    retargets to the latest sub-dir (receipt, binding, `failure_note`). The fit's pass choice reads the latest sub-dir.
  - The seal scan's `Wave.run_dirs` lists `attempt-k` sub-dirs for every phase base and the bundle (readers never get
    attempts).
  - One NAV run-dir enumeration (`nav_run_dirs` x `attempt_dirs`) serves both research_cycle and verify.
  - Gap: the exe is not compared (m-cycle_resume).
- Concurrency:
  - Over-commit (major) and lock robustness (minor) above.
  - research_cycle pairs are safe. Partners share no output: ref||u (NAV vs u-out + candidate cache) and card||marginal
    (u-out/admission vs cache/pool).
  - Start checks and logs run in the main thread in pair order. A failed key step raises before the partner's finish.
    The partner's complete output is then reused on resume by the same done criteria, so nothing incomplete reads as
    done. The fit is never paired, and nothing runs past `--stop-after`.
- Ledger integrity:
  - `ledger_append` changes touch the lock only. The appended bytes are unchanged (asserted).
  - Keep-verdict copies are created exclusively and never overwritten.
  - Code-keyed reuse only refuses.
  - The content digest never makes a stale receipt fresh; at worst the reverse.
- The four task-0 minors, each fixed:
  - (1) Unlink retried about 2 s on PermissionError, then a warning, never a raise (`test_ledger_lock_permission_and_unlink`).
  - (2) At timeout the message names the holder's pid and whether it is running (pid + create_time); the permission
    case is reported separately.
  - (3) verify uses `CR.completed_exe_sha256` / `completed_exes` with MAX_ATTEMPTS, the same as research_cycle; the test
    plants `-run1` / `-run10`.
  - (4) The spec-kind assertion now derives the expected kind from the sibling generated spec / template schema, which
    is falsifiable.
- Scope: every file is in E1's list. research_cycle.py is under P3, plus its test (fake floor-kill behaviours). The
  backtest_integrity.py edit is limited to `ledger_append`'s lock (ruling of progress.md:65), and test_research_spec.py
  to minor 4. No specs, C++ or atx-db/ files are touched. No Python copy of a C++ rule was added.
- Blindness: the lane's and the reviewer's commands use synthetic fixtures only. No 2020-2023 return / IC / NAV output
  and nothing dated 2024-01-01 or later was opened.

## Checked
- [x] .agents/cpp/agent.md section 10 checklist applied to the diff (Python only: error paths, determinism, tests vs contract)
- [x] Diff stays inside brief's files-in-scope (cross-lane edits listed and ruled)
- [x] Evidence in report matches claims (scripts/tests re-run under seed 0; fake-wave identity re-run against 3fa2dd4a)
