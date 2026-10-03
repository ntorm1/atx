# Lane E1 re-review 1 (fix round 1)

## Verdict
BLOCK: 1 new major, 1 new minor.
- The round-1 major (admission over-commit) is closed.
- E1-REUSE (b) is closed.
- E1-REUSE (a) closes the stale-exe hole but was made unconditional. It now refuses the parent-inherited IC passes of
  every NAV-only template (the rule-cell path) after any IC exe rebuild. That path ran at e40c9152.
- Fixing that, by scoping the check or by a PM ruling that accepts the refusal, is enough to approve.

## Reviewed SHA
Head `124e2f4e` (code `210d5de8`), FIX_BASE `e40c9152`, identity base `3fa2dd4a`.
Commits:
- 210d5de8: admission under the claims lock.
- e26292e4: E1-REUSE (a), executable check on reuse.
- c422313e: E1-REUSE (b), import closure.
- 124e2f4e: report.

Diff `e40c9152..124e2f4e`: 7 files, +477/-51.

## Evidence
1. `scripts/tests` in pool-17 at 124e2f4e:
   `PYTHONHASHSEED=0 PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests`
   gave exit 0:
   ```
   .........................................................                [100%]
   341 passed, 4 skipped in 272.49s (0:04:32)
   EXIT=0
   ```
   Afterwards the tree was clean and HEAD was still 124e2f4e.
2. Over-commit repro re-run against the new `RB.admit`. Setup: shared claims dir, 3584 MiB free, budget 12000, u
   cap 2560, ref cap 1536, floor 512, distinct live owners.
   ```
   1. ref waits (timed out): free memory 3584 MiB less 2560 MiB the other claims have yet to allocate < 2048 MiB (peak 1536 + floor 512)
   2. simultaneous trials with both admitted: 0 of 20
   3. lone runner admitted after 1 check(s); reserved_by_others 0
   claims left: []
   ```
3. Flag-absent identity: the fake wave (wave_fixture world, no `driver` key; plain and with the PM8-15 marginal
   ruling), 3fa2dd4a vs 124e2f4e:
   - argv: 22/22 and 23/23 identical.
   - driver log: 57/57 and 59/59 lines identical after normalising the world path and SHAs.
   - files: the same 95 / 96, with 0 files differing after normalising the world path, time keys, git SHAs and the
     receipt digests that hash time.
   - Identity holds.
4. NAV-only template probe on the `test_research_cycle` fake tools:
   - Setup: the parent cell runs; its bounded receipts get the real runner's K-P9-10 keys (argv_sha256, command,
     executable_sha256); the child template sets only `nav.output`; `bin/ic.exe` is rebuilt.
   - At e40c9152 the child runs: exit 0.
   - At 124e2f4e the child stops with
     `3 HARD-STOP [u]: u output out/U-1 was made by an executable with sha256 dae7f3a0... (out/U-run1/receipt.json); bin/ic.e...`.
5. Windows `os.replace` onto a claim file that another handle has open for reading raises
   `PermissionError` (winerror 5) in the scratchpad.

## Findings (round 1, status)
- `run_bounded_research.py:220-233` major (admission over-commit): **addressed**.
  - The free check runs inside the claims lock, net of other claims' cap minus their tree RSS (evidence 2).
  - No deadlock: there is one lock with a 30 s bound, and a waiting runner holds no claim.
  - A lone runner is never starved: with no claims, the reservation is 0, the same as before.
  - A waiter is admitted once `F0 - others' caps >= need`, and otherwise times out into an AUTO outcome.
  - An orphaned child keeps its share: the claim stays live while the runner or its adopted child lives. It is
    dropped at the next check once both are gone, so no claim leaks forever. The test covers runner kill and then
    child kill.
- `cycle_resume.py:113-124` minor, raised to E1-REUSE (a): **addressed** (exe compared on every K-P9-10 receipt;
  pre-K-P9-10 receipts and receipts without executable_sha256 are reused as before; tested). Over-broad: see N1.
- `wave_stage_cell.py:73-78` minor, raised to E1-REUSE (b): **addressed**.
  - The transitive in-repo closure (importer dir and CODE_DIRS, function-level imports, the `_engine_module` loader)
    is diffed against the receipt's `source_sha`.
  - No `source_sha` means not checked, as before.
  - It applies to every reader / bundle receipt, which the E1-STALE ruling (no wave in flight at merge) covers.
- Open, deferred by the PM:
  - HostClaims lock robustness (`run_bounded_research.py:167-183`).
  - Sticky `exes_sha256` (`research_cycle.py:876`).
  - Content digest with nested time (`stage_chain.py:93-96`).
  - Judge-parallel ordering (`wave_stage_record.py:162`).
  - K-P9-10 `attempt` / `build_type` semantics (`research_cycle.py:944`).

## New findings
N1. `scripts/cycle_resume.py:138` (with `scripts/research_cycle.py:1954`) | major

**Problem.** E1-REUSE (a) is unconditional. It refuses (exit 3) any done bounded step whose K-P9-10 receipt names
another executable SHA than the exe on disk now.
- research_spec's documented template rule (module doc :30-33) is that "a NAV-only change reuses the parent's u, fit,
  card and w passes". Those steps are the parent's outputs and receipts, so a rule cell can never run once the IC exe
  has been rebuilt since its parent ran.
- That is the normal P9 cadence: root rebuilds per wave, and S1, D1 and S2 change the IC exe. Evidence 4 shows the
  child ran at e40c9152 (exit 0) and stops at 124e2f4e (exit 3).
- The only remedy is `--suffix`. It recomputes u / fit / w on the new exe, which changes the IC evidence the NAV-only
  cell is supposed to share with its parent. This is a flag-absent behaviour change that no ruling weighed. E1-REUSE
  covers "after a re-lock".
- `test_resume_refuses_exe_mismatch` pins only the single-spec case.

**Fix.** Pick one:
- Scope (a) to the case the ruling describes: when the spec carries `exes_sha256` (`lock --exes`), refuse a receipt
  whose `executable_sha256` differs from that exe key's pin. That stays opt-in and still closes the re-lock hole.
- Exempt outputs a template inherits unchanged from its parent, which are covered by the parent's receipts and pins.
- Get a PM ruling that NAV-only cells re-run the IC passes after an IC rebuild, and update research_spec's reuse rule
  and the rule-cell flow to match.

In every case, add a test with a NAV-only template after an IC rebuild.

N2. `scripts/run_bounded_research.py:250-258` (with `:426`) | minor

**Problem.** `adopt()` rewrites the claim with `os.replace` outside the claims lock. Another runner's `live()` reads
claim files under that lock, and on Windows a replace onto a file open for reading raises `PermissionError`
(evidence 5).
- The window is correlated in the designed path. The losing runner of a simultaneous pair takes the lock within
  about 50 ms of the winner's release and reads the winner's claim, while the winner is in `Popen` and then `adopt`.
- The exception escapes in `main` after the launch. The `except BaseException` branch then records `runner-error`,
  which is not an AUTO outcome, and kills the just-launched child, so the step hard-stops.

**Fix.** Take the claims lock in `adopt()` (as `try_claim` does), or retry the replace on `PermissionError` as
`remove()` does. Never let an `adopt` failure kill the child: on failure, warn and keep the runner-pid claim.

## Checked
- [x] Diff stays inside E1's files (cycle_resume, research_cycle under P3, run_bounded_research, wave_manifest,
  wave_stage_cell, test_wave_driver, report)
- [x] New tests test the contract:
  - two launches at one instant (barrier): exactly one admitted; orphan share; stale drop.
  - exe mismatch: u and nav; legacy receipts reused.
  - import closure: depth 2, function-level import, committed after the run, no source_sha.
  - Gap: no NAV-only-template case (N1).
- [x] Evidence in report matches claims: tests re-run under seed 0; fake-wave identity re-run against 3fa2dd4a.
- [x] Blind: synthetic fixtures only; no 2020-2023 output and nothing dated 2024-01-01 or later opened.
