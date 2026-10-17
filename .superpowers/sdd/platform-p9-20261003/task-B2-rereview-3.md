# Lane B2 re-review 3 (fix round 2, ruling B2-N37)

## Verdict

**APPROVE**: 0 Required, 2 Suggested.

- **Spec verdict:** every item of `r10-dispatch.md` "B2-FIX2" (1-5) is done, under rulings B2-N37 (cause (b)),
  B2-N37-B6, B2-FIX2-EXE and TEST-1. B2-MERGE-MSG is accepted as it stands.
- **Quality verdict:** good.
  - The only code change in range is the test `atx-impl/tools/test_nav_summ_v8.py::test_legacy_n37_numbers_reproduced`
    and its two helpers.
  - The comparison is 1e-12 relative on floats. Keys, lengths, types, strings, booleans, nulls and integers compare
    exactly.
  - The test pins exactly one degenerate row.
  - The golden, every expected hash and every pinned file are untouched.
  - My own teeth show that the tolerance is the stated one and is load-bearing, and that every structural check bites.
- **Merges:** all 38 merge commits in `46ffe3dc..509f4b79` are plain (tree = `git merge-tree --write-tree`
  recompute). This includes the lane's two merges, `e4683013` and `96ff5aa0`.

## Reviewed SHAs

- HEAD `509f4b79` (pool-14, `feat/p9-b2-20261003`, tree clean). BASE `46ffe3dc`: the re-review 2 APPROVE commit
  (REV-BASE / REV-FRESH; `task-B2-rereview-2.md` reviewed `53856c8e`, and its docs commit is `46ffe3dc`).
- **Range, first parent:**
  - `46ffe3dc -> 73b4acfd`: a fast-forward to root's M2 docs head (reflog "Fast-forward"). This is root's history,
    which contains root's merge `5dfb0ba1` of `46ffe3dc`.
  - `71abeb2f`: the fix, test only.
  - `e4683013`: merge of root `094bcd3b`.
  - `eef8aabd`: records.
  - `96ff5aa0`: merge of root `858a4fca`.
  - `509f4b79`: records.
- `git diff 858a4fca 509f4b79` (the lane against the root head it merged last):
  - outside `.superpowers` it is exactly one file, `atx-impl/tools/test_nav_summ_v8.py`;
  - that file's diff is byte-identical to `git diff 73b4acfd 71abeb2f` (`cmp`);
  - the blob at HEAD is `03ea6071`, the same as at `71abeb2f`.
- Root's history in range was read as one-line subjects only.

## Spec compliance (`r10-dispatch.md` "B2-FIX2"; ledger B2-N37, B2-N37-B6, B2-FIX2-EXE, B2-R1, B2-6, B2-MERGE-MSG)

| item | state | evidence |
|---|---|---|
| 1. Merge root at the end; configure `equity-dev`; ONE Debug build `p9-b2-r` of `atx-research-eval` | done | `e4683013` (root `094bcd3b`) and `96ff5aa0` (root `858a4fca`). Receipt `b2-evidence/mega-p9-b2-r-receipt.json`: source `73b4acfd`, dirty 0, target `atx-research-eval` only, exit 0, verb `71d19288...91c7`. No second build (cause (b): no C++) |
| 2. Reproduce with `ATX_EQUITY_ROOT` = pool-2 `build-equity` (read only) and `ATX_EQUITY_BIN` = own bin, blind flags; diff in root's `n37_diag.py` form, copied into `b2-evidence/` | done | `r10-pyt-n37-repro.log`: unchanged test, 1 failed. `r10-n37_diag-root-m2.py` is root's tool verbatim (my SHA-256 of both: `80e54b23ab77...`, equal). The lane's `r10-n37-diag.py` prints paths, counts, booleans, digit-count histograms, max relative difference and ulps (read in full: no value printed) |
| 3. Cause (b): golden UNCHANGED; numeric compare at 1e-12 relative (B2-R1 (b) form); docstring names B2-N37 and carries the B2-6 disclosure; report states the max observed difference | done | `test_nav_summ_v8.py:67-71` (comment naming B2-N37, `N37_REL_TOL = 1e-12`, `N37_REFUSED`). Docstring `:125-130`: B2-N37, "golden file is unchanged", and the B2-6 disclosure. Report "Fix round 2": max relative difference 4.967e-14 (summ), 3.104e-16 (PBO). The cause (a) exclusion is argued from the nlohmann round-trip printer: 0 of 26,188 tokens shorter than Python's repr |
| B2-N37-B6: the docstring states that the legacy argv lists the reference as a dir, so exactly one degenerate self-pair row is expected (the pin of 1) | done | `:127-128` "the n37 argv lists its reference among the dirs, so exactly one row is a self pair, degenerate under B2-R1 (c)"; pin `:167` `assert degenerate == 1` |
| 4. Tests: the test file (no C++, so no gtest) | done | `r10-pyt-*`: fixed n37 1 passed; whole file with root's inputs 10 passed, on `73b4acfd`+fix, `e4683013` and `96ff5aa0`; synthetic 9 passed / 1 skipped on all three |
| 5. Report section, handoff head line, commit, object tree deleted | done | `task-B2-report.md:1065-1216`, `handoff-B2.md:3-40`. pool-14 `build-equity/bin` is absent (only receipts, `audits/` and one render dir remain) |
| B2-MERGE-MSG | accepted (ruling) | `e4683013` carries git's default message without the trailer. Not a finding |

## (1) Merges in range

| merge | parents | real tree | recompute | verdict |
|---|---|---|---|---|
| `e4683013` (lane: root `094bcd3b`) | `71abeb2f` `094bcd3b` | `73c2c567` | `73c2c567` | plain |
| `96ff5aa0` (lane: root `858a4fca`) | `eef8aabd` `858a4fca` | `d64e548a` | `d64e548a` | plain |
| `5dfb0ba1` (root: B2 `46ffe3dc`) | | `7167e614` | `7167e614` | plain |
| `27e69387` (root: T2) | | `fcef6848` | `fcef6848` | plain |
| `b182dfdd` / `08f04e07` / `898bf120` (root M3: C3b / D3W / ABS) | | `49159e4f` / `a9f66958` / `639a26a5` | equal | plain |
| `0a2ceb2b` / `7f6f1c75` / `858a4fca` (root M4: D3 / OD3P / AL-CLOCK) | | `2ce6ea7a` / `36c83320` / `868ae10e` | equal | plain |
| every other merge in `git log --merges 46ffe3dc..509f4b79` (the other lanes' own root merges) | | | | 38 / 38 plain |

## (2) The code change (`71abeb2f`; `atx-impl/tools/test_nav_summ_v8.py`)

- **`numeric_mismatches` `:74-92`.** It recurses over dicts (same key set, else `<path> (keys)`) and lists (same
  length, else `(length)`). For leaves, `type(got) is not type(want)` gives `(type)`: this separates int and float,
  bool and int, null and float, str and number. Floats compare with
  `math.isclose(rel_tol=1e-12, abs_tol=0.0)`, which is symmetric and relative to the larger magnitude; NaN equals
  NaN. Every other leaf compares with `==`. The function returns paths only.
- **`take_degenerate_disclosure` `:95-118`.** It acts only on a row whose engine `paired` block carries
  `degenerate`. That flag must be `True` and absent from the golden, with no `one_minus_rho` in the golden. The run's
  `one_minus_rho` must be a float below 1e-6. Each refused key (`memmel_se`, `t`, `lw.se`) must be null in the run
  and a float in the golden. Those keys are popped from both rows, and the rest of the row is still compared by
  `numeric_mismatches`; my probe P6c shows a kept paired float in the self pair is still compared.
- **The test body, `:138-139` and `:158-173`.**
  - The golden's canonical-dump check is now a boolean assert. The old string-equality assert would have printed a
    string diff of the golden.
  - Per row, the disclosure runs, then the numeric compare. Problems are listed as `[i]<path>` and asserted empty, and
    `degenerate == 1` is pinned.
  - The PBO goes through the same `numeric_mismatches`.
  - The assertion messages carry `problems[:20]` (paths) and the tolerance constant only.
- **B2-R1 (b) form.** A flat 1e-12 relative is at least as strict as B2-R1 (b)'s `1e-12 / (1 - rho)` for paired
  floats (1 - rho <= 1), and it is exactly what B2-N37 orders. The observed maximum, 4.967e-14 at 360 ulps, leaves a
  margin of about 20x.
- No other line of the file changed: the diff has three hunks, all inside the n37 section. The longest new line is
  `:173` at 119 columns (house Python limit 120). The G-P5 guard skips test files by design
  (`test_no_python_mirror.py` docstring), so the helpers are no mirror question.

## (3) No golden, expected hash or pinned file changed

- The lane's own commits touch `atx-impl/tools/test_nav_summ_v8.py` and sprint records only:
  - `71abeb2f`: the test;
  - `eef8aabd`, `509f4b79`: `.superpowers/sdd/platform-p9-20261003/**`;
  - the merges are plain.
- **Whole range** `git diff --name-only 46ffe3dc 509f4b79`, filtered for golden / fixture / expected / pin / hash /
  `build-equity`: besides sprint logs, only `atx-impl/tests/fixtures/composition_rules_list.json` (ABS `265bb427`,
  via root merge `898bf120`) and `scripts/tests/wave_fixture.py` (C3b `278fc2d8`, via `b182dfdd`). Both are other
  lanes' reviewed commits that reached the branch through root's merges. Neither is B2's, and neither is on the n37
  path.
- The n37 golden (`build-equity/mega-nav-v71-summ-n37.json`, `-pbo-n37.json`) is untracked: pool-2's `build-equity/`
  is git-ignored, so git cannot show a change. I read file metadata only, without opening the files: the mtime of
  both is 2026-09-29 07:06, before this round. The lane states that it wrote nothing under pool-2: its temp dirs were
  in its scratch, and nav_summ's `EngineStats` uses `tempfile.mkdtemp`. Nothing in the diff writes there.

## (4) The tolerance cannot hide a real difference: my own teeth (TEETH-RESTORE)

- **Tool.** A scratch script (`teeth_rr3.py`, my session scratchpad; not committed). It works on synthetic rows only:
  37 made-up rows, with row 0 a self pair in the engine's disclosed form. It loads the test module from disk and
  calls `take_degenerate_disclosure` and `numeric_mismatches` exactly as `:158-167` and `:172` do. A probe "FAILS"
  when the problem list is non-empty or the degenerate count is not 1.
- **Restore checks.** Before and after each mutation the tool asserted three things:
  - the file's SHA-256 (LF form, since `core.autocrlf=true` and the working file is CRLF) equals HEAD's blob
    `f762d1ed0e1b5d81...`;
  - `git hash-object` equals `HEAD:<path>` `03ea6071`;
  - `git status --porcelain` is empty.
- **Restore method.** The tool keeps the pre-mutation bytes itself, writes the mutation, and restores the kept bytes in
  `finally` (restored == kept: True). It never used `git checkout` / `restore` / `stash`.
- **Outcome.** Every check passed: pre, after M1, after M2 and final. No permission refusal.

| probe (synthetic) | HEAD bytes | M1: `N37_REL_TOL = 1e-6` on disk | M2: key-set check removed on disk |
|---|---|---|---|
| P0 identical rows, self pair disclosed | passes | passes | passes |
| P1 a float leaf x (1 + 1e-9) | FAILS (1 path) | **passes** (the loosened tolerance hides it) | FAILS |
| P2 / P2b a float leaf x (1 + 3e-12) / x (1 + 5e-13) | FAILS / passes (the tolerance is 1e-12, not looser) | passes / passes | - |
| P3 a planted extra key | FAILS `(keys)` | FAILS | **passes** (the removed check was the only guard) |
| P3b a missing key | FAILS `(keys)` | FAILS | raises KeyError (still a failure) |
| P4 a changed string | FAILS | FAILS | - |
| P5 int -> equal float; bool flipped; null -> float; a longer list | FAILS (type / value / type / length) | FAILS (all four) | - |
| P5e 0.0 vs 1e-300; P5g NaN vs a number; P5f NaN vs NaN | FAILS; FAILS; passes (by rule) | - | - |
| P6 self pair with 1 - rho = 1e-3; a refused key not null | FAILS; FAILS | FAILS; FAILS | - |
| P6c self pair: a KEPT paired float x (1 + 1e-9) | FAILS | passes | - |
| P6d a second degenerate row | FAILS (degenerate 2: the pin of 1) | FAILS | - |
| P6e the degenerate flag removed | FAILS (degenerate 0, `(keys)`) | - | - |
| PBO: identical; float x (1 + 1e-9); extra key | passes; FAILS; FAILS | passes; passes; FAILS | passes; FAILS; passes |

What the table shows:
- On HEAD bytes, the probe set catches every planted difference that is not inside 1e-12.
- Loosening the tolerance to 1e-6 lets the planted 1e-9 float difference through, in the rows, in the self pair and in
  the PBO. So the 1e-12 constant is what catches it, and a loosened tolerance is detectable.
- With the tolerance loosened, every non-float check still fails as it should: missing / extra key, changed string,
  type, bool, list length, the degenerate-form checks and the pin of 1. The tolerance governs float leaves only.
- Removing the key-set check lets a planted extra key pass, in a row and in the PBO. So that check is load-bearing.
- These results agree with the lane's own in-memory teeth on the real rows (`r10-n37-diag-fixed.txt`: 8 / 8 caught,
  5e-13 accepted). Mine add the mutation side.

## (5) Blind

- I read in full: the report's "Fix round 2" section, the handoff's R10 section, the commit messages of `71abeb2f`,
  `e4683013`, `eef8aabd`, `96ff5aa0` and `509f4b79`, both diagnosis outputs, the diagnosis tool, all eight
  `r10-pyt-*` logs and the receipt.
  - They carry paths, counts, booleans, generic JSON paths, digit-count histograms, the max relative difference
    (4.967e-14 / 3.104e-16) and ulps (360 / 2).
  - There is no 2020-2023 statistic.
- I scanned the build, configure and waiter logs and the cache-stats log (including the UTF-16 `mega-p9-b2-r-build.log`)
  for statistic names: no match except the false positive "boost-static-assert".
- **Self-disclosure.** A `git grep` I ran for the golden's file name over HEAD's tracked files printed matching lines
  of the tracked doc `docs/plans/2026-09-29-mega-alpha-v7-pitch.html`. That doc is outside the range and not a lane
  record, and one of the lines carries a v7.1 in-sample figure. I did not copy it anywhere, it plays no part in this
  review, and I opened nothing else of that kind. Reported for the PM.

## Evidence

### My own runs

All runs in pool-14 with `C:/Program Files/Python312/python.exe`, `-p no:cacheprovider`, my own `--basetemp`, and
`PYTHONDONTWRITEBYTECODE=1`. `ATX_EQUITY_ROOT` and `ATX_EQUITY_BIN` were unset. The tree was clean after each run.

| run | result |
|---|---|
| `atx-impl/tools/test_nav_summ_v8.py` (synthetic) | 3 passed, 6 failed, 1 skipped. The skip is the n37 test (no `ATX_EQUITY_ROOT`). **All 6 failures are the missing verb:** pool-14 has no `build-equity/bin` (the lane deleted its object tree by rule), and nav_summ fails closed with `SystemExit: nav_summ: no engine statistics executable ...`, 6 / 6. The six tests (`test_year_table`, `test_bundle_verdict`, `test_origin_class_in_ledger_line`, `test_variance_from_rerun_cells`, `test_protocol_line_is_chained_but_not_counted`, `test_v8_bootstrap_defaults`) are byte-unchanged by the diff. With the lane's `p9-b2-r` verb they pass 9 / 1 skipped: from the lane's logs only (`r10-pyt-v8-file-synthetic{,-merged,-merged2}.log`) |
| teeth `teeth_rr3.py` | above: 22 probes on HEAD bytes, 18 under M1, 7 under M2; every outcome as expected; restore assertions all True |
| pytest introspection probe (a 2-test scratch file, made-up values; for S-1) | under default `--tb`, `assert len(rows) == 37` prints `where 2 = len([...rows...])`, and `assert main(argv) == 0` prints the argv and the captured stdout |
| merges | 38 / 38 plain (table above) |

### From the lane's logs only

EXE-CUR applies to these: the verb is `p9-b2-r`, SHA-256 `71d19288...91c7`, source `73b4acfd`, receipt dirty 0, and
pool-14 has no exe left.
- The n37 test: red reproduced on the unchanged test (1 failed), then 1 passed with the fix.
- The whole file with root's inputs: 10 passed on `73b4acfd`+fix, `e4683013` and `96ff5aa0`.
- The diagnosis numbers (counts, paths, max relative difference, ulps).
- The lane's merged-tree runs used a verb whose source predates D3W's eval-verb hunks:
  - `git diff 73b4acfd 509f4b79` changes `eval_report.cpp`, `eval_cli.cpp` and two eval headers;
  - the new `--vol` flag is absent from the n37 argv (D3W-EVAL: flag absent, bytes unchanged);
  - the n37 Python path (`nav_summ.py`, `backtest_integrity.py`, `test_nav_summ.py`, `research_window.py`) is
    unchanged in `71abeb2f..509f4b79`.

  The lane disclosed this (TEST-1: one build). By EXE-CUR, the pass at HEAD on HEAD's verb is root's to show (below).

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `atx-impl/tools/test_nav_summ_v8.py:130` (docstring "A failure names JSON paths only, never a value"), with `:154` and `:157`; `task-B2-report.md` "The fix", last line ("value-free even without `--tb=no`") | Suggested (S-1) | The comparisons the fix added are value-free. Two pre-existing lines are not, under pytest's default flags (my probe): `:157` `assert len(got_rows) == len(want_rows) == 37` prints a truncated repr of the real rows if the count differs. `:154` `assert NS.main(argv) == 0` prints the argv and, because `capsys` has not been read yet, nav_summ's captured stdout. So the docstring's sentence and the report's "even without `--tb=no`" overstate it. Blindness still holds in practice, because every real-data run uses `--tb=no --show-capture=no -rN` (B2-FIX2-EXE, `handoff-root-m2.md`) | `rc = NS.main(argv)`; `capsys.readouterr()`; `assert rc == 0, "nav_summ exit"`; `n_got, n_want = len(got_rows), len(want_rows)`; `assert n_got == n_want == 37`. Or narrow the sentence to "the row comparisons name JSON paths only; run with the blind flags". Correct the report line at the next record edit |
| `atx-impl/tools/test_nav_summ_v8.py:108-109`, `:167` | Suggested (S-2) | The pin is a COUNT (`degenerate == 1`). B2-N37-B6 says the one degenerate row is the self pair, but the test never checks that the degenerate row is the reference's dir; the lane's diagnosis does (`r10-n37-diag.py`: `Path(row['dir']).resolve() == (equity.parent / ref).resolve()`, True). `omr < 1e-6` also admits any negative value. Not exploitable in practice: the self pair always has rho = 1, so it is always degenerate, and a second degenerate row makes the count 2 (my probe P6d) | Assert that the degenerate row's `dir` resolves to the argv's `--reference` token, and use `abs(omr) < 1e-6` |

No Required finding. Nothing in the range is wrong against B2-N37 / B2-N37-B6 / B2-FIX2-EXE or unsafe to merge.

## What root must check at merge (M5, second B2 merge; MERGE-SHA = the commit that adds this file, above `509f4b79`)

1. **The merge is plain.** HEAD already holds root `858a4fca`. If root has moved since, `git merge-tree --write-tree`
   recomputes the tree, and the lane's only code file, `atx-impl/tools/test_nav_summ_v8.py`, has no other writer in
   range.
2. **No golden moved.** `git diff <root-before> <merge> --stat` shows no golden / expected-hash / pinned file. In
   pool-2, `build-equity/mega-nav-v71-summ-n37.json` and `-pbo-n37.json` must be byte-unchanged: root compares their
   SHA-256 with its own record; the mtime is still 2026-09-29.
3. **The n37 test on root's own current Debug verb (EXE-CUR).** Run
   `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity` with root's Debug `atx-research-eval`, built from a source whose
   `git diff <source>..<merge>` over the eval verb's C++ / CMake is empty. Name the receipt and the SHA-256
   (SPDLOG-RELINK). The command is
   `pytest atx-impl/tools/test_nav_summ_v8.py --tb=no --show-capture=no -rN -p no:cacheprovider`.
   - It must give 10 passed, with the n37 test passing.
   - This **clears the known-red set** (B2-N37: exactly this test), which becomes EMPTY.
   - A failure is a stop for B2, never a tolerance change.
   - This is the first run of the fix on a verb that includes D3W's eval hunks; the lane's runs used `p9-b2-r` from
     `73b4acfd`.
4. **At the gate.** If the n37 test is run on a Release verb, a difference beyond 1e-12 is a red to attribute
   (B2-R1 (b) allows `1e-12 / (1 - rho)` for paired floats only by ruling), not a reason to loosen
   `N37_REL_TOL`.
5. **Nothing else to re-prove.** There is no C++, CMake, flag, JSON key, rule id or `atx.<name>/v<n>` literal in the
   round (SQL2-CLS: nothing new), and no cross-lane edit. The synthetic cases of `test_nav_summ_v8.py` need a built
   verb; they ran 9 / 1 skipped in the lane.

## Rules kept

- Python only: no build, no configure.
- No file of the lane was edited. The two teeth mutations were restored byte-identically and checked by SHA-256,
  hash-object and an empty porcelain.
- No other pool was written. Pool-2 was read for one tool file (`m2-logs/tools/n37_diag.py`, SHA-256 only), the
  sprint contracts, and the two golden files' metadata (not opened).
- No `ATX_EQUITY_ROOT` was set, nothing dated 2024 or later was opened, `holdout_gate.py` was not run, and the trial
  ledger was not opened.
- No `git checkout` / `restore` / `stash`. No permission refusal.
