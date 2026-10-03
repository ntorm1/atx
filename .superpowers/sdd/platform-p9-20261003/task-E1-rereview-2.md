# Lane E1 re-review 2 (fix round 2)

## Verdict
APPROVE. 0 blockers and 0 majors open.
- N1 is addressed by af1038d0 (ruling E1-REUSE-a2).
- N2 is addressed by a830deca.
- Two new minors, both in opt-in paths and fail-closed or report-only: N3 and N4.
- The five round-0 minors stay deferred by the PM.

## Reviewed SHA
Head `fdd2bda3` (code `a830deca`), FIX_BASE `124e2f4e`, identity base `3fa2dd4a`.
Diff `124e2f4e..fdd2bda3`: 8 files, +415/-56.

## Evidence
1. `scripts/tests` in pool-17 at fdd2bda3:
   `PYTHONHASHSEED=0 PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests`
   gave exit 0:
   ```
   344 passed, 4 skipped in 275.77s (0:04:35)
   EXIT=0
   ```
   Afterwards the tree was clean and HEAD was still fdd2bda3.
2. NAV-only template probe on the `test_research_cycle` fake tools. Setup: the parent's bounded receipts carry the
   real runner's K-P9-10 keys; the child sets only `nav.output`; `bin/ic.exe` is rebuilt.
   ```
   1a unpinned child after IC rebuild: exit 0; u reused: True; receipt exe NOTE: u ..., NOTE: w ...
   1b cell-pinned child (parent unpinned): exit 0; u reused: True; receipt exe NOTE: u ..., NOTE: w ...
   1c child before re-lock: PIN MISMATCH exe ic ...   (inherited pins, round-0 minor "sticky exes_sha256")
   1c parent-pinned child (re-locked): exit 0; u reused: True (judged by the parent's pin = receipt exe; no NOTE)
   2a parent re-locked after rebuild, resumes its own u: REFUSED exit 3: HARD-STOP [u] ... made by an executable with sha256 dae7f3a0...
   2b child of a re-locked parent (inherited u): REFUSED exit 3: HARD-STOP [u] ...
   3a P (NAV-only of pinned G) after rebuild: exit 0; u reused: True
   3b C (NAV-only of P, same u as G): REFUSED exit 3: HARD-STOP [u] ...   (see N3)
   ```
3. Flag-absent identity: the fake wave (wave_fixture world, no `driver` key; plain and with the PM8-15 marginal
   ruling), fdd2bda3 against both 3fa2dd4a and 124e2f4e:
   - argv: 22/22 and 23/23 identical.
   - driver log: 57/57 and 59/59 lines identical after normalising the world path and SHAs.
   - files: the same 95 / 96, with 0 files differing after normalising the world path, time keys, git SHAs and the
     receipt digests that hash time.
   - `exe_notes` and "Exe notes" appear in no file.

## Findings (status)
- N1 `cycle_resume.py:138` major (unconditional exe refusal broke NAV-only templates): **addressed**.
  - A refusal now happens only against an `exes_sha256` pin: the parent's resolved pin for an output the template
    inherits (`Cycle.inherited` / `reuse_pin`, research_cycle.py:918-936), else the spec's.
  - An unpinned mismatch is reused, with a `receipt exe NOTE:` log line, the run stage's `exe_notes` (a log line, the
    05-run receipt and the wave-result cell block) and a wave-log line.
  - Probes: the unpinned, cell-pinned and parent-pinned cases all run (exit 0), and a pinned stale receipt refuses,
    both for the spec's own output and for an inherited one (evidence 2).
  - Pre-K-P9-10 receipts, and receipts without `executable_sha256`, are reused silently as before (tested).
- N2 `run_bounded_research.py:250-258` minor (adopt replace race): **addressed**.
  - `adopt()` takes the claims lock (5 s; without it, a retried replace still writes the file whole).
  - It retries `os.replace` for 2 s.
  - Any failure warns and keeps the runner-pid claim. The child is never stopped.
  - The lock is held only for the write and released in `finally`, never across the child's lifetime.
  - No claim leaks: release, or the stale drop once the runner and the child have both gone, removes it. A temp file
    is removed when the replace finally fails.
  - The only residue is a `.claim.tmp` left by a runner hard-killed mid-write, which the `*.claim` glob ignores.
  - The Windows test holds the claim open: the write succeeds once the file is released, and while it stays held,
    adopt warns, the child keeps running, and no lock or temp file is left.
- Round-1 items, still **addressed**: the admission major (210d5de8) and E1-REUSE (b) (c422313e).
- **Open, deferred by the PM:**
  - HostClaims lock robustness (`run_bounded_research.py:167-183`).
  - Sticky `exes_sha256` (`research_cycle.py:876`; evidence 2, case 1c).
  - Content digest with nested time (`stage_chain.py:93-96`).
  - Judge-parallel ordering (`wave_stage_record.py:162`).
  - K-P9-10 `attempt` / `build_type` semantics (`research_cycle.py:944`).

## New findings
N3. `scripts/research_cycle.py:918-936` | minor

**Problem.** `inherited()` and `reuse_pin()` look only one level up the template chain, but an output can be
inherited across several generations. Case: G (pinned at exe A) made u; then the IC exe is rebuilt. P is a NAV-only
template of G, locked with `--exes` (pin B). P runs fine, because its u is judged by G's pin A. C is a NAV-only
template of P. C's u has the same name, so it is judged by P's resolved pin B, and C refuses (exit 3) the very output
P legitimately reused (evidence 2, case 3b). This fails closed and needs `exes_sha256` pins (lock_exes, opt-in) plus
an IC rebuild between G and P. It is likely on a chain of accepted rule cells.

**Fix.** Judge an inherited output by the pin of the ancestor that introduced it: walk up while the phase's output
name, out_root and suffix-free state are unchanged, and use the last such ancestor's resolved pin. Add a depth-2
chain test.

N4. `scripts/wave_stage_util.py:110` (with `wave_stage_cell.py:47-51` and `wave_result.py:217`) | minor

**Problem.** `exe_notes` compares each receipt's `executable_sha256` with the exe on disk, without the pin rule
research_cycle applies. In the parent-pinned case (evidence 2, case 1c), research_cycle reuses the inherited u / w on
a pin match and logs no NOTE. The wave still records `exe_notes` for them, and the wave log labels them "reused on a
rebuilt exe without an exes_sha256 pin", which is false there. This is report-only; no bytes are reused that
research_cycle would refuse.

**Fix.** Either label the notes neutrally ("made by another exe than on disk now"), or apply the same `reuse_pin`
rule and note only unpinned mismatches. Keep "no mismatch means no key", as now.

## Checked
- [x] Diff stays inside E1's files (cycle_resume, research_cycle under P3, run_bounded_research, wave_result,
  wave_stage_cell, wave_stage_util, test_wave_driver, report).
- [x] The new tests test the contract:
  - Pinned refuses / unpinned notes on one spec, including legacy receipts.
  - A NAV-only template after an IC rebuild in the unpinned, cell-pinned, parent-pinned and parent-pin-moved cases,
    with only the cell's NAV run.
  - The run-stage note and its absence: no key, no line.
  - Adopt under a held-open claim (Windows).
  - Gaps: no depth-2 chain test (N3); the parent-pinned case is not checked at the wave level (N4).
- [x] Evidence in report matches claims: tests re-run under seed 0; fake-wave identity re-run against 3fa2dd4a and
  124e2f4e.
- [x] Blind: synthetic fixtures only; no 2020-2023 output and nothing dated 2024-01-01 or later opened.
