# Lane B2 re-review 2 (post-identity redo: B2-11, PRE-15, G-P5, D2R-2 (b), B2-14 merges)

## Verdict
**APPROVE**: 0 Required, 5 Suggested.
- Spec verdict: every item of `r7-dispatch.md` "B2" (1-6), `r8-dispatch.md` "B2" (0-4) and `r9-dispatch.md` "B2" (1-5)
  is done. The whole-suite runs are deferred to the gate (TEST-1); the lane's own runs on `52cdcc8c` stay as its
  evidence.
- Quality verdict: good (section "Quality").
- B2-14: all six merges are clean.
  - `c37ec671` has 7 conflict regions in 5 files. Each was resolved by a region edit. None was resolved by taking a
    whole side, except the lane-owned sprint records, where the lane kept its own newer text. All 4 B2-R3 hunks of the
    old `research_cycle.py` were re-placed byte for byte on E2's split.
  - `b1df1829`, `c3201a60`, `7832719f`, `1b15330d` and `52cdcc8c` are plain merges (tree equals git's recomputed
    tree).

## Reviewed SHAs
- HEAD `53856c8e` (pool-14, `feat/p9-b2-20261003`, tree clean). BASE `26fe8d50` (REV-BASE: the head re-review 1
  names as reviewed).
- The lane's own code commits in range: `fe152fca` (PRE-15 + re-review 1 S2/S3/S5/S6/S7), `d8f80c7a` (B2-11),
  `34838462` (D2R-2 (b)). Plus the resolution hunks of merge `c37ec671`.
- The rest of `26fe8d50..53856c8e` is root's history through six merges, read as one-line subjects only.
- `git diff 52cdcc8c 53856c8e` and `git diff 1b15330d 52cdcc8c` outside `.superpowers` are both empty. So the code at
  HEAD is the code that Debug `p9-b2-q` (source `52cdcc8c`) and Release `p9-b2-p` (source `1b15330d`) built.
- Against root `685e238b` the lane differs in 35 code / test files, as the report says.

## Earlier findings (re-review 1, `task-B2-rereview-1.md`)

| finding | state | where (HEAD) | evidence |
|---|---|---|---|
| **R1**: verify read the summ's verb hash before the summ ran | **CLOSED** (`d8f80c7a`) | `scripts/wave_stage_record.py:103-124` (`eval_pin`, `eval_problem`): key + pin only, keyed on `wave_kinds.is_v2`, reads nothing the summ writes; verify's call `:46` | `test_wave_eval_verb.py::test_a_named_and_pinned_v2_cell_passes_verify...` runs a real v2 manifest (`new-nav-rule.json`) in stage order and asserts `summ.json` / `bundle.json` do not exist at verify; `::test_verify_refuses_a_v2_cell_without_exes_eval_or_its_pin` (unnamed, unpinned: exit 4). My run: passed |
| **R2**: the judge's bundle ran a default verb, and nothing compared it | **CLOSED** (`d8f80c7a`) | `judge_verb` `:127-143` is called at `:268`, before `score()` (`:281` / side-by-side): it checks the key, the pin, and the verb on disk against the pin (EXIT_PIN). `bundle_once` `:204-238`: `--engine-stats verb[0]`, and `CV.check_verb` on `bundle.json`, fresh or reused (`:222-226`). `scored_verdict` `:241-259`: `CV.check_summ_verb` on `summ.json` / `pbo.json` (`:256`). `wave_steps.py:127-136` `bundle_argv(..., verb)`. Both judge branches pass `verb` | the 4 judge tests (both branches; fresh / reused; moved bytes; summ / pbo of another verb), my run passed. My teeth (below) caught the removal of the pin check |
| S1 (preflight refusal) | open: left by the lane (report "Suggested left"; needs a rule for "the template will name it") | - | see S-4 |
| S2 (6 ulps) | closed | `research_eval_edges_test.cpp:377` | - |
| S3 (101 columns) | closed | no C++ line over 100 columns in the four touched C++ files (my awk scan) | - |
| S4 (classes.json) | closed: already in root | `classes.json` rows `eval-output`, `ledger-head`, `test-golden` | - |
| S5 (G-3c names its verb) | closed | `book_diagnostics.py:554-562` `engine_producer`, result key `engine` | from the lane's logs (needs the verb) |
| S6 (no assertion on Acklam) | closed | `research_eval_edges_test.cpp:411-427` (printed only) | - |
| S7 (the constant wins) | closed | `EvalEdges.CscvConstantWinsAndTiesTheZeroColumnByIndex` (`:327`) | lane's `r9-gtest-q-eval-whole.log` OK |
| D2R-2 (b) | **DONE** (`34838462`) | `atx-impl/tools/nav_summ.py:1065` `allow_abbrev=False` (nav_summ's only parser); `test_research_spec.py:874-957` | my `-k e27b` run: 3 passed. My own scan (5,014 tracked JSON files outside `atx-db/` / sprint dir / build, 541 string lists under a `summ` path): 0 tokens that are not an exact nav_summ option |
| PRE-15 | **PRESENT AND TESTED** (`fe152fca`) | `eval_report.cpp:169-200` (`tot_minus_m` = `dsr_at_json(m, n_tot - campaign_registry, V)`, whole ledger only `:581`; `--hand` prefix off `:603`), text `:733-737`, `:813`; `dsr_total.py:20-25, 107-125`; `nav_summ.py:91-99` | gtest `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns` (`research_eval_tie_test.cpp:331`), from the lane's logs only (`r9-gtest-q-eval-whole.log`, Debug `p9-b2-q`, 26 / 26); pytest `test_dsr_total.py` (4 PRE-15 cases) needs the verb, from the lane's logs only. N = N_tot - M is tied to the Python fixture's trial breakdown; the DSR at that N to `dsr_at` |
| G-P5 deletion on the merged tree | **HOLDS** | `test_no_python_mirror.py` ALLOWLIST: the six B2 statistics rows are gone (`c37ec671` kept the deletion and root's rows); "16 mirrored rule function(s) in 7 row(s) left" | my run: 7 passed. With no verb in the tree, every statistic route refuses ("nav_summ: no engine statistics executable ...") in 23 of my tools tests, so no Python fallback survives. `test_a_bin_without_the_verb_fails_closed` needs the built verb for its first half: from the lane's logs only (`r8-gp5-failclosed-o.log`, 1 passed) |
| `exes.eval` named and pinned per v2 cell (B2-10 (c) under B2-11) | **DONE** | verify `:110-124`, judge `:127-143` | see R1 / R2 |

## B2-14: conflict regions and merges

Method:
- `git merge-tree --write-tree <merge>^1 <merge>^2` recomputes each merge. A plain merge's tree must equal the
  recomputed tree.
- For `c37ec671`, the recomputed tree (`f3b958f9`, with conflict markers) was diffed against the real one
  (`3ef45476`). The diff touches only the 5 conflicted files plus `scripts/cycle/{cli,spec,phases}.py`, where the
  re-placed hunks sit.
- Each side was checked against the merge base `1b25b9a4` (`git diff 1b25b9a4 <merge>^1|^2 -- <path>`) and against
  the result (`git diff <merge>^1|^2 <merge> -- <path>`).

| merge | file | region (conflicted file) | resolution | verdict |
|---|---|---|---|---|
| `c37ec671` | `scripts/research_cycle.py` | 1: module docstring (lines 4-195; ours = 186-line usage text = base + B2-R3's `exes.eval` entry; root = E2's 3-line shim text) | root's text; B2's entry re-placed byte for byte at `scripts/cycle/cli.py:54-59` | OK (B2-14's shim clause; nothing dropped) |
| `c37ec671` | `scripts/research_cycle.py` | 2: code body (lines 203-2338; ours = old module + B2-R3's 3 code hunks; root = re-exports) | root's re-exports + one line `from cycle.spec import EVAL_EXE` (`research_cycle.py:40`). `EVAL_EXE` goes to `cycle/spec.py:81`, `effective_exes` docstring + `names` line to `spec.py:416-428`, the `summ_step` 3 lines + import to `cycle/phases.py:21, 736-738`. `git diff 1b25b9a4 c37ec671^1 -- scripts/research_cycle.py` has exactly these 4 hunks, and `git diff c37ec671^2 c37ec671 -- scripts/cycle` is exactly them | OK |
| `c37ec671` | `scripts/tests/test_no_python_mirror.py` | ALLOWLIST head (one region) | combined: ours' deletion of the six B2 rows + its comment line; root's D2-3 comment. Root's D3 / P10 retargets, D2-5 comment, A2-note row and `FROZEN_PAIRS` entry merged outside the region and survive (`git diff ^2 merge` = only the B2 rows) | OK |
| `c37ec671` | `scripts/wave_stage_record.py` | verify's `problems` line | combined: root's `exe_problem(exe, pins, SK.rule_cell_kind(w))` + B2's `eval_problem(w, done)` | OK |
| `c37ec671` | sprint `handoff-B2.md` | 1 region (lines 3-21) | ours (root held an older copy of the lane's own record) | OK (lane-owned record) |
| `c37ec671` | sprint `task-B2-report.md` | 2 regions (status line 7-12; the tail section 562-737) | ours | OK (lane-owned record) |
| `c37ec671` | everything else | auto-merged (`test_research_cycle.py` exes dicts, `atx-engine/tests/CMakeLists.txt`, ...) | git's merge | OK: real tree = recomputed tree outside the files above |
| `b1df1829` | - | no conflict | tree `8493fff6` = recomputed | plain |
| `c3201a60` | - | no conflict | tree `0938e907` = recomputed | plain |
| `7832719f` | - | no conflict | tree `b67d4cb5` = recomputed | plain |
| `1b15330d` | - | no conflict | tree `3b5d7971` = recomputed | plain |
| `52cdcc8c` | - | no conflict | tree `5e0fd662` = recomputed | plain |

The lane's tables say "4 files, 6 regions"; the count is 5 files, 7 regions (S-2). This is a record count only. Every
region is listed and correct.

## Traces (r8-review-notes "B2")

- **The judge never publishes statistics from another verb.** The judge's "Statistics of record" are `paired` / `dsr`
  / `pbo` (from `cycle_verdict.json`, built from `summ.json` / `pbo.json`) and `bundle` (from `bundle.json`).
  - `judge_verb` (`:268`) runs first. It fails closed (exit 3) on a missing key or pin, or on a verb on disk whose
    SHA-256 is not the pin. Nothing has run at that point; the cell's summ also writes its ledger line, so this is
    where it must stop.
  - `scored_verdict` re-checks every `summ.json` row and `pbo.json` against the pin (`CV.check_summ_verb`, the
    verdict's own rule). It does this in both branches, after the summ and before the verdict is used.
  - `bundle_once` runs nav_summ with `--engine-stats <path>` (cwd = root, so a root-relative path resolves; nav_summ's
    `bundle` calls `engine_of(args)`). It then `CV.check_verb`s `bundle.json`, both when the bundle has just run and
    when a stale one is reused.
  - `marginal` (the marginal exe) and `book` (the NAV exe's summaries) come from no eval verb.
  - v1: `judge_verb` returns None and the argv is unchanged.
- **The summ's verb.** `Cycle.__init__` replaces `spec["exes"]` with `effective_exes(spec)` when a build key is
  present (`cycle/phases.py:139-141`). So `summ_step` (`:736-738`) passes the same path `lock --exes` hashed and
  `judge_verb` checked.
  - One consequence: for a build-key spec with a summ, `effective_exes` always yields `eval`. There, verify's
    "lacks exes.eval" branch can only fire without a build key, and the pin is the operative check. That is the B2-R3
    design re-review 1 accepted, not a finding.
- **Fixture-only fix in E2's fake world** (`d8f80c7a`):
  - `wave_fixture.py:401-415` `pin_eval_verb` is called only from `test_wave_kinds.world()` (`:171`, D3's file, one
    line). No production rule changed (`eval_problem` / `judge_verb` were not touched by it).
  - `wave_fixture.py:363-366`: the fake `add_alpha` copies the parent's `exes` / `exes_sha256`, exactly as the real
    `research_add_alpha.derive_spec` does (`copy.deepcopy(parent)`, no pop of either key).
  - v1 worlds: the parent's `exes` equal `cell_spec`'s and carry no pins, so nothing changes.
  - The v1 plan reproduced byte-equal (below), and `test_wave_driver.py` (v1) passes 28 / 28.
  - No rule weakened.
- **`nav_summ.py` `allow_abbrev=False`** at `:1065` (the only parser).

## Evidence (my own runs in pool-14; `C:/Program Files/Python312/python.exe`, `-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`, ATX_EQUITY_BIN unset; tree clean after each)

| run | result |
|---|---|
| `scripts/tests/test_wave_eval_verb.py` + `test_no_python_mirror.py` | 14 passed (7 + 7; G-P5 "16 ... in 7 row(s)") |
| `scripts/tests/test_wave_kinds.py` | 112 passed |
| `scripts/tests/test_wave_driver.py` (the v1 consumer of the judge / verify code) | 28 passed |
| `scripts/tests/test_cycle_scoring.py` | 3 passed, 2 failed: `test_verdict_dsr_is_the_ledger_variance_value...` and `test_the_summ_runs_the_named_verb_and_the_verdict_refuses_another` need the built verb (FileNotFoundError / "no engine statistics executable"); the lane's object tree is deleted. From the lane's logs only |
| `scripts/tests/test_research_spec.py -k e27b` | 3 passed (84 deselected) |
| `scripts/tests/test_research_cycle.py` + `research_ledger` fixture test | 97 passed, 3 skipped (RESEARCH_CYCLE_LIVE_ROOT) |
| `test_nav_summ_engine.py`, `test_dsr_total.py`, `test_book_diagnostics.py`, eval_tie + research_eval_edges fixture tests | 29 passed, 23 failed. **Every failure is the missing verb**: nav_summ refuses with "no engine statistics executable ..." (49 occurrences in the log). This is fail-closed behaviour; the statistics assertions are from the lane's logs only (`r8-pytest-tools-{o,p}-*`) |
| v1 plan identity: `b2-evidence/r7-plan-v1.py` on one synthetic root, planned by three `git archive` trees: root `685e238b`, pre-B2-11 `b1df1829`, HEAD | 33 lines each, byte-equal (`cmp`); the v1 bundle line carries no `--engine-stats` |
| `git merge-tree --write-tree` of HEAD with root `685e238b`, D3 `b9b288e5`, D3W `dcddb01d`, C3b `98bba064` | no textual conflict with any of them (pairwise) |

There is no `test_wave_stage_record*.py` in the tree; the judge / verify code is covered by `test_wave_eval_verb.py`,
`test_wave_kinds.py` and `test_wave_driver.py`. No gtest was run by me: every C++ claim is from the lane's logs only
(`r9-gtest-q-{eval-whole,ledger-whole,impl-ledger}.log`: 26 / 16 / 29 passed; receipt `mega-p9-b2-q-receipt.json`:
source `52cdcc8c`, dirty 0, verb `80a73375...c0142d`).

**Teeth (TEETH-RESTORE; no permission refusal).**
- Tool: a scratch Python script (`teeth_rr2.py`). It checked the tree was clean and `git hash-object` equal to HEAD's
  blob `d203f7ed`, kept the pre-mutation bytes, and replaced `scripts/wave_stage_record.py:139` `    if disk != pin:`
  with `    if False:  # TEETH-RR2`. It ran the two tests below, restored the bytes in `finally`, and asserted the bytes,
  the blob id and an empty `git status --porcelain`.
- Result:
  - `test_the_judge_refuses_a_verb_whose_bytes_moved` **FAILED**. With the check off, the cell's summ ran on the moved
    verb, and only `scored_verdict` stopped it afterwards, so "nothing ran" was violated.
  - The control `test_a_named_and_pinned_v2_cell_passes_verify_and_the_judge_runs_the_pinned_verb` passed.
  - Restored sha256 `36fc46ed...` = pre-mutation bytes, and the tree is clean.
- Conclusion: the pre-summ check has teeth, and the `scored_verdict` check is real defence in depth.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `scripts/wave_stage_record.py:303`; `scripts/tests/test_wave_eval_verb.py:147, 155` | Suggested (S-1) | 121 / 121 / 122 columns. The house Python limit is 120 (E2-18 wrapped `wave_stage_kinds.py:421` for one column) | wrap; whitespace only |
| `task-B2-report.md` "Follow-up 1" table (row "sprint-dir handoff-B2.md, task-B2-report.md (2 regions)") and the R9 conflict table (row `c37ec671`: "4 files, 6 regions") | Suggested (S-2) | the count is 5 files / 7 regions (handoff 1, report 2) | correct the count at the next record edit |
| `scripts/wave_stage_record.py:151-155` (`verb_refusal`) with `scripts/cycle_verdict.py:93-95` | Suggested (S-3) | for a reused `bundle.json` of another verb, the refusal ends "run the summ again". But the judge never re-runs a bundle whose `bundle.json` exists: the remedy is to remove `bundle.json`, as the test does at `test_wave_eval_verb.py:201` | add a bundle-specific remedy to the bundle refusal ("remove `<out>`; the next judge runs the bundle on the pinned verb") |
| re-review 1 S1 (`wave_stage_preflight.py`) | Suggested (S-4) | a v2 wave whose cell spec cannot name and pin `exes.eval` is still refused only at verify, after a library wave's screen stage has ledgered admission trials | P10, or the follow-up after B2-10 (b)'s re-pin (the lane's own deferral) |
| `atx-impl/tools/nav_summ.py:485` (`sharpe`), `:579` (`year_table`), `bundle`'s per-year dSR `:630-635` | Suggested (S-5, P10; outside this range and not on the G-P5 list) | a descriptive annualised Sharpe and per-year dSR are still computed in Python (`bundle.json` `years`, printed tables). The judge does not consume them; they predate the lane | a P10 candidate under the owner's centralization priority: print them from the verb |

## Quality
- **Modular / reusable.**
  - One verb rule (`cycle_verdict.check_verb` / `check_summ_verb`) serves the cycle's verdict and the wave's judge
    (bundle and summ). The wave adds only a (path, pin) resolver `eval_pin`, shared by verify, judge and plan, plus two
    small adapters (`pinned_verb`, `verb_refusal`).
  - `bundle_argv` gains one optional argument with v1 bytes unchanged.
  - PRE-15 is one row through the existing `dsr_at_json` / `trial_breakdown`, with no new statistic code path.
  - The fake-world fix is a faithful model of `derive_spec`, not a test bypass.
- **No Python mirror of a C++ statistic survives in B2's scope.**
  - The six G-P5 rows are retired and the guard passes.
  - nav_summ, dsr_total and book_diagnostics compute nothing of record without the verb and refuse without it.
  - `frozen_statistics.py` is a test oracle under `atx-engine/tests/fixtures/`. No production module imports it
    (`backtest_integrity.py:52` only names it in its docstring), and the guard skips fixture dirs by design.
- **Residual (S-5):** descriptive Sharpe / per-year dSR in nav_summ, outside B2's list.
- **Performance:** no hot path. The judge adds one SHA-256 of the verb per judge.
- **Determinism:** no iteration-order or thread-order change. The side-by-side branch passes the same `verb`.

## What root must check at merge
1. **Merge.** `git merge 53856c8e` into root `685e238b` is clean (B2 already holds `685e238b`). Pairwise against D3
   `b9b288e5`, D3W `dcddb01d` and C3b `98bba064`, `git merge-tree` shows no textual conflict today. Any later conflict
   is resolved region by region (B2-14).
2. **Keep-both points.**
   - **D3**, `scripts/tests/test_wave_kinds.py:171`: B2's one line `F.pin_eval_verb(root)` in `world()` must survive
     D3's rewrite. D3's `world()` is at its `:184-186` and every D3 v2 test builds through `world()`. Without the line,
     E2's v2 tests that reach verify are refused by B2-10 (c) / B2-11.
   - **D3W / B2**, `atx-engine/src/research/eval/eval_report.cpp` (B2-EVAL-KEEP): the hunks are adjacent but disjoint.
     B2 has `:569-605`, `cell_text` `:722-737` and `:798-813`; D3W has `:603-628`, `:634-660`, `:683`, `cell_text`
     `:710-716`. After both merges root runs the eval gtests (26) and
     `EvalVerb.HandRowAtNTotMinusMLeavesOutTheCampaigns`.
   - **C3b**, `scripts/cycle/spec.py`: at C3b's head `98bba064` C3b has no edit there yet (its D2R-2 (a) text waits for
     B2). C3b does edit `scripts/tests/test_wave_kinds.py` (pm6-6 variant dropped; `record_spec`) and
     `scripts/tests/wave_fixture.py` (`gross_match` default `pm6-6-engine`; `FakeCycle.calibrate`). Both auto-merge
     with B2's lines. After both, root runs `test_wave_kinds.py` + `test_wave_eval_verb.py`: under the engine match,
     `match.cell_spec` is the `-cal` run recipe, which `eval_problem` / `judge_verb` read, and it must inherit the eval
     pin.
3. **TRAIN identity procedure** (report "What root re-proves at merge" item 3, flag-absent nav_summ, B2-1 / B2-7). On
   X-5 and Y-F0, each with the merged head's verb (build tag, SHA-256 and source commit stated; SPDLOG-RELINK: name the
   receipt), run
   `nav_summ.py <dir> --reference <its base> --protocol v8 --psr --dsr-ledger <COPY of trials.jsonl> --dsr-total <same COPY> --json a.json`.
   Rules for the run:
   - never `--ledger`;
   - read-only ledger flags get a COPY;
   - the reference is never among the dirs.

   Compare against root's identity outputs at `1b25b9a4` / `c2416d62`:
   - stdout equal except ONE added line per dir, `   DSR_hand (N_tot - M=..., ruling PRE-15): ...`;
   - `a.json` within 1e-12 after popping `deflated_total.tot_minus_m` and `nav_summ_run`;
   - the same argv without `--dsr-total`: stdout, stderr and the JSON without `nav_summ_run` byte-equal;
   - `--dsr-hand` (not in the argv) changes two words per line ("prefix rule");
   - D2R-2 (b) changes nothing on this argv;
   - before the run, measure rho per B2-13; follow B2-7 for any differing printed line or bootstrap count.

   PRE-15's public cross-check is the report's "State for root" item 2: Y-F0's `tot_minus_m` must reproduce the
   R0-12 logged DSR_hand at N 123. A miss is a stop for the PM.
4. **ROOT-1a targeted checks.**
   - A Debug target-scoped build of `atx-research-eval`, `atx-engine-research-eval-tests`,
     `atx-engine-research-ledger-tests` and `atx-impl-tests`.
   - The lane's filters: the eval and ledger targets' cases, and `atx-impl-tests --gtest_filter=TrialLedger*`.
   - With the built verb: the lane's pytest files that I could not run (the 23 tools cases, the two
     `test_cycle_scoring.py` verb tests, `test_nav_summ_engine.py::test_a_bin_without_the_verb_fails_closed`).
   - Whole suites, Release and canaries at the gate (TEST-1). `$canaryExes` already lists `atx-research-eval`
     (`scripts/research-build.ps1:73`).
5. **SQL2-CLS:** B2 adds no `atx.<name>/v<n>` literal. Its added lines name only `atx.research-wave/v1` / `v2`, both
   already in `classes.json` row `wave-manifest`; `atx.research-eval/v1`, `atx.ledger-head/v1` and the two fixture
   classes are registered.
6. **B2-10 (b)** at the W1-I2 (b) re-pin: add `exes.eval` (path + SHA-256) to the P9 lineage parents. Until then a
   real v2 wave without `driver.lock_exes` and a named verb is refused at verify (intended, fail closed).

Blind and data rules: synthetic fixtures only. No 2020-2023 statistic was read, nothing dated 2024 or later was
opened, and the trial ledger was not opened. No build was run, no file of the lane was edited (the one teeth mutation
was restored byte-identically), and no other pool was touched; other lanes' heads were read as git objects only.
