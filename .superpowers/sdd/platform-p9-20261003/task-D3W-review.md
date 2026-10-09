# Lane D3W review (P9 RESUME 9; first review; fresh adversarial reviewer)

## Verdict

**APPROVE.** 0 Required, 10 Suggested.

- Merge conditions are listed in "What root must check at merge".
- Ruling TEST-1 (owner; amends r8 item 5 and the reviewers' "run yourself" lines) defers several runs to the gate (GATE-1):
  - the whole `scripts/tests` under seeds 0 and 1;
  - the stage chain;
  - the whole gtest targets;
  - every Release build.
- The lane's seed-1 run was voided under ENV-1. It is now "deferred to the gate (TEST-1)": pending by rule, not a finding.

## Reviewed SHA

- HEAD `1e6c4336` (`feat/p9-d3w-20261005`, `C:/atx-wt/pool-23`), the code head.
- BASE `fd084e78`, from REV-BASE: `git merge-base 1e6c4336 feat/platform-p9-20261003`.
  - BASE is root's code head.
  - Root's docs head `685e238b` (docs only) is not merged; the `@@ROOT3@@` placeholder stays (r9 item 0).
- DIRTY-1a: my first act was commit `01770e76`, `docs(d3w): handoff as it stood at STOP 8 (DIRTY-1)`.
  - It changes `handoff-D3W.md` only, committed exactly as it stood.
  - I edited no lane record after that commit.
- Every merge in `BASE..HEAD` is plain: its tree equals `git merge-tree --write-tree` of its parents.
  - `c7549d6a` (root `fd084e78`): `62e967a9`
  - `cec719a0` (root `1b64ce45`): `cfd2c2d8`
  - `8363656e` (root `a15ad887`): `884054b2`
  - `899b5811`: `af8807b9`
  - `f2ff4954`: `98612431`
- `git diff fd084e78 1e6c4336` contains the lane's own hunks only: 31 code / test files, plus the sprint dir.
- EXE-CUR holds for my C++ runs.
  - The handoff says the object trees were deleted. They were NOT (see S-1).
  - `build-equity-rel/bin` holds tag `p9-d3w-h` (Release, source `c7549d6a`). All 7 exes match the receipt's SHA-256 (my hash).
  - `git diff --name-only c7549d6a 1e6c4336` over `*.cpp *.hpp *.h CMakeLists.txt *.cmake CMakePresets.json` is empty: `1e6c4336` adds a Python test and the sprint-dir logs only.

## Spec verdict (r7-dispatch "D3W" (1)-(6), r8-dispatch "D3W" 1-6, r8-review-notes "D3W", r9 "D3W-REVIEW", rulings)

| item | verdict | basis |
|---|---|---|
| r9-0 DIRTY-1a | DONE (reviewer) | `01770e76` |
| r7-(1) / r8-1 merge root | DONE | See (a) below. |
| r7-(2) C++ walk-forward fit (K-P9-8, D3-1) + gtests | DONE | See (b) below. |
| r7-(3) runner reads v3; `WeightsV3.SingleRowEqualsV1Bytes`; refusals | DONE | See (c) below. |
| r7-(4) fitter flags + post-step; D3-2 names; no edit in `fit` / `fit_prior` | DONE | See (d) below. |
| r7-(5) ALCOMB-7, then ALCOMB-6 (+ D3W-EVAL) | DONE | See (e) below. |
| r7-(6) D2-9 (S-1 test, S-2 `check_factor_series`, S-c strict) | DONE | See (f) below. |
| r7-(6) D2R-3 S1 / S2 | DONE | See (g) below. |
| Flag absent = every byte unchanged (ALCOMB-4 / -5) | DONE, with the disclosed code-identity fields | See (h) below. |
| r8-4 ABS-1 (extension point named) | DONE | Report "For ABS". It suffices for the composition half; see "For ABS" below. |
| r8-5 Release once; pytest groups; suites; re-merge root | DONE for what TEST-1 leaves; the rest deferred to the gate (TEST-1) | See (i) below. |
| r8-6 report / handoff | PARTIAL | See (j) below. |
| D3-1 constants; flat start | DONE | See (k) below. |
| PRE-35 (D3W's half: v3 + `SingleRowEqualsV1Bytes`) | DONE | The walk-forward kind wiring is D3's (pool-20). |
| PRE-38 | untouched | The realised vol ratio stays the NAV exe's `ann_vol`. No NAV file is in the diff. |
| D3W-PB | out of scope | "For part B" is present (report :188-197). |
| D3W-STALE | noted, not a finding | `scripts/specs/p9/rules/mom-volman-v1.json:50` still says `--engine-fit` is refused with `--mom-volman`. Nothing under `scripts/specs` is in the diff; the report flags it (:179-180). |
| Scope / cross-lane | DONE | See (l) below. |

**(a) Merge root (r7-(1) / r8-1).**
- `cec719a0` (`1b64ce45`) and `c7549d6a` (`fd084e78`), both plain.
- Root's code has not moved since then.

**(b) C++ walk-forward fit (r7-(2)).**
- `fit.hpp:191-226`; `fit.cpp:379-439`.
- The window of row j is the TRAIN decisions d with d + lag <= j (`fit.cpp:417-422`).
- `fit_composition` reads factor values only through the TRAIN mask:
  - ic-shrink: `train_mean`, `:71-78`.
  - theme-erc covariance: `:106-111`.
  - The std rule reads no data.
  - So no row reads a decision after j - lag (verified by reading).
- Refusals, `:384-397`: the window, lag < 2, step 0, min < 2, tsmom / resid, no filled window, more than 4096 rows, and a refused row, which names its decision and session.
- CLI: parse `composition_cli.cpp:363-406`; the block is written only with the flag (`:323-326`); counts at most 9 digits (`:210-219`).
- Gtests `WalkForward.*`: 4 / 4 (my run).

**(c) Runner reads v3 (r7-(3)).**
- `strategy_ic_admission.cpp:29-33` (v3 plus a `static_assert` against the engine id), `:700-719`.
  - v1 and v2 refuse the block; v3 requires it.
- `strategy_ic_rules.cpp:695-786` `composition_walk_forward`:
  - exactly 6 keys;
  - 1..4096 rows;
  - `from_session` strictly increasing (`:734`);
  - every library id present, finite and >= 0;
  - at least one weight per row;
  - refused beside redistribution, residualise and theme-tsmom.
- `walk_forward_stages` `:622-672`. `set_weight_rows` `strategy_ic_composition.cpp:533-579`.
- The row in force per date comes from `lower_bound` (`first_at_or_after`), so no row is applied before its session.
- The book is flat before the first row:
  - the plain path skips w = 0 dates (`:362`);
  - standardise uses a zero-mass block at date 0 (`rules.cpp:640-660`).
- Gtests `WeightsV3.*` 3 / 3 and `CompositionWeightRows` 1 / 1 (my run).
- "Marginal keeps refusing v3": `strategy_marginal_ic.cpp:267` is unchanged and accepts v1 / v2 only. Verified by reading; no test pins v3 there.

**(d) Fitter flags and post-step (r7-(4)).**
- Flags `composition_engine_fit.py:171-183`. They are D3-2's names and equal D3's `scripts/wave_kinds.py:86-90` (pool-20 `3834bc0c`), including the refused set and `--engine-fit`.
- `check_walk_forward` `:202`; `POST_ONLY` `:231`; `run_post_step` `:761`.
- The fitter's only hunk is `main()` (`fit_composition_weights.py:2817-2824`).
- `OUTPUT_PRODUCERS` fingerprint is `d28a3089...` at BASE and at HEAD (my run of `d3w-fingerprints.py` on a `git archive` of BASE).

**(e) ALCOMB-7, then ALCOMB-6 (r7-(5)).**
- ALCOMB-7:
  - `fit.cpp:142-181`: `theme_sleeve` is now shared with `theme_tsmom_schedule`, with the same operation order; the tsmom fixture gtests pass.
  - `:274-277` (refusal), `:371-375`.
  - `composition_cli.cpp:182-185`, `:286`.
  - Fitter: check 3 plus `engine_sleeve`.
- ALCOMB-6:
  - `strategy_ic_rules.cpp:788-823`; `strategy_ic_runner.cpp:706-707`.
  - D3W-EVAL `--vol`: `eval_report.cpp:613-620`, `:640-642`, `:658-660`; `eval_cli.cpp:118`.
- Gtests: `MomVolman.*` (engine 3, IC 7) and `EvalVerb.VolFlagAddsNetVolOnly`, all green (my run).

**(f) D2-9 (r7-(6)).**
- S-1: the branches of `test_compare_admission_csv` (`test_composition_engine_fit.py:642-656`) plus `test_run_exe_refusals` (`:658`).
- S-2: `check_factor_series` reads through `_read` / `_json` and requires object rows (`composition_engine_fit.py:252-276`); test `:675`.
- S-c: `compare_values(strict=)` (`:469-486`); `composition_resid.py:253` passes `strict=True`; tests `:604`, `:617`.
- My teeth R-T2 bites.

**(g) D2R-3 S1 / S2 (r7-(6)).**
- `test_fit_composition_weights.py:750-753`.
- `scripts/tests/test_research_spec.py:940-941` (listed as cross-lane).

**(h) Flag absent = every byte unchanged.**
- My fitter identity run reproduces the lane's. 17 of 21 files are byte-identical. The 4 weights files differ only in two code-identity fields:
  - `provenance.engine_fit.producer_fingerprint` (`--engine-fit` runs);
  - `provenance.resid.producer_fingerprint` (`--theme-resid` runs). D2-9 S-c mandates the `parent_check` edit, so this field moves even with no D3W flag (root check 3).
- IC side:
  - the whole `atx-impl-strategy-ic-tests` passes, 231 / 231, with no pre-existing mom-volman, theme-tsmom or theme-cov-hedge test edited;
  - one pre-existing line changed: `strategy_ic_runner_test.cpp:873-874` now uses v4 as the unknown schema, a contract change and not a pinned value;
  - a mom-volman IC run gains two new files (root check 4).

**(i) Release build and suites (r8-5): DONE for what TEST-1 leaves; the whole suites are deferred to the gate.**
- Release `p9-d3w-h` was built (7 targets, receipt, exit 0).
- Fitter / composition pytest: 305 passed (lane, at head).
- `scripts/tests` seed 0: 519 passed, 5 skipped, 0 failed (lane, on the `c7549d6a` tree; `1e6c4336` touches no `scripts/` file).
- Stage chain: 10 / 10.
- `scripts/tests` seed 1 is **VOID under ENV-1**:
  - `d3w-logs/r8-scripts-tests-seed1.log` ended 2026-10-07 17:30, after the stop: 177 failed, 342 passed, after 21 hours.
  - The failures are 154 `git init` exits 3221225794, 10 `HARD-STOP ... exit 3221225794`, and the rest inside the same tests' child chains. Example: `test_e27b_refuses_every_spelling_at_add_alpha` is red because `ic.cmd --plan-only exit 3221225794`.
  - The ENV-1 re-run was never made, and the result is in no record.
  - Under TEST-1 this run is **deferred to the gate (GATE-1)**: pending by rule.
  - I did not run it. TEST-1 forbids it, and three other `scripts/tests` suites were already running on the host.
- "Merge root's then-current head again": root's code has not moved.

**(j) Report and handoff (r8-6): PARTIAL.**
- Present: items with file:line, test and log; the identity procedure; literals / flags / keys; cross-lane edits; For PRE; For ABS; For part B. The file:line anchors I checked are exact.
- 8 placeholders are unfilled (S-1).

**(k) D3-1 constants and flat start.**
- `fit.hpp:199-204`: lag 2, step 21, min_decisions 21, at most 4096 rows.
- Flat before the first row: see (c).

**(l) Scope and cross-lane edits.**
- Listed cross-lane edits: `strategy_ic_mom_volman.cpp:147-150`, its test `:347`, `test_research_spec.py:941`, and the five eval files (D3W-EVAL).
- Every other file is inside D3-2's scope.

## Quality verdict (owner priority: modular / reusable / performant C++ in atx-engine and atx-impl)

**Sound, with small debts (S-2 .. S-5).**

**One fit entry.**
- `fit_walk_forward` is a loop over the one `fit_composition` on a masked view. No rule has a second implementation.
- `run_fit` takes an optional `WalkForwardSpec`, so it stays one verb.
- The new `theme_sleeve` removes a duplicate: one sleeve formula now serves theme-tsmom-v1 and mom-volman-v1.

**No duplicated window logic in the runner.**
- The runner never recomputes a window. It maps `from_session` to a date index through one helper, `first_at_or_after`, which theme-tsmom's block mapping now shares.

**v3 reader bounded; hot path allocation-free.**
- The parse is bounded: at most 4096 rows, one weight per library id, in a SHA-pinned file.
- `set_weight_rows` allocates once, before the first `add`.
- `add` / `add_standardised` do one `row_of[d]` / `row_weights[r][index]` lookup per date and none per cell, and allocate nothing.
- The DetPool path reads only const state. `CompositionWeightRows` shows that 2 and 3 workers equal the serial result bit for bit.

**No Python mirror of a C++ rule.**
- The fitter checks the C++ rows for shape only (`walk_forward_rows`) and never recomputes a window.
- The only Python arithmetic added is check 3's reference sleeve (`volman_sleeve`). It is built from the existing `composition_theme_tsmom.sleeves`, which is D2's P12 pattern.
- `test_fit_walk_forward.py` `python_rows` is a test-only stand-in for the executable, also D2's pattern.
- `weights_v3` repeats one runner refusal (S-8).

**Safety.**
- No UB found:
  - counts are at most 9 digits, so `cut + lag` cannot overflow;
  - the stage-bytes division is reached only after `count >= 1` is checked;
  - every refusal comes before any output;
  - `set_weight_rows` keeps nothing on refusal (tested).
- Debug and Release built clean under `/W4 /WX` (receipts exit 0).
- New standard headers are included where used.

**Performance.**
- Each row's fit rebuilds the signed series, O(members x decisions) per row. This is fit-time only, at most 4096 rows; acceptable.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `task-D3W-report.md:5,7,8,27,31,106,118,224`; `handoff-D3W.md:9,12` | Suggested (S-1) | The report keeps 8 placeholders, among them OUTCOME, HEAD, TESTS and PERM. "Tests (final head)" is empty. The handoff says the object trees were "deleted by name at the close", but `build-equity/` (2.5 GB; Debug, holding the teeth-MUTATED `p9-d3w-f` / `-g` binaries) and `build-equity-rel/` (1.7 GB; the valid `p9-d3w-h`) are both present. | Fill the report at the next lane touch. Until then, root reads the facts from this review and `d3w-logs/`. Delete both trees by name when the PM no longer needs `p9-d3w-h`. |
| `atx-engine/include/atx/engine/research/composition/fit.hpp:174-177` vs `atx-impl/src/strategy_ic_mom_volman.hpp:36-41` | Suggested (S-2) | mom-volman-v1's rule id, theme, decision bounds and 128 (= lookback 126 + lag 2) are defined twice, in two layers, with nothing binding them. `strategy_ic_rules.cpp` now includes both. The lane used the right pattern for the schema id (`strategy_ic_admission.cpp:32`) but not here. | Make the engine's constants canonical and have atx-impl use them, or at least `static_assert` the equalities in `strategy_ic_mom_volman.cpp`. |
| `atx-impl/src/strategy_ic_composition.cpp:347-351`, `:419-423` | Suggested (S-3) | The same `weight_at` lambda appears in `add` and `add_standardised`. The report's "For ABS" already has to say "a per-date absent skip goes in both". | Make it one private `Impl` member, `weight_at(index, d)`, so a per-date extension has one place. |
| `atx-impl/src/strategy_ic_rules.cpp:695-786` | Suggested (S-4) | `composition_walk_forward` is about 92 lines and does three jobs: block shape, row parse, and lifted-candidate sign / theme. House style asks for about 60 lines or fewer. | Split it into three helpers. Behaviour unchanged. |
| `atx-impl/src/strategy_ic_rules.cpp:788-823` | Suggested (S-5) | `write_mom_volman_sleeves` puts mom-volman-specific file I/O in the rule-table TU. It belongs beside `composition_volman` (`strategy_ic_mom_volman.cpp`). Placing it here avoided a larger cross-lane edit. | Move it when that file is next owned. |
| report "For PRE" (:171-178) | Suggested (S-6) | Two facts PRE needs are unstated. (1) The sleeve files hold only the recorded TRAIN sleeve: its sessions are the fit's decisions, its values are 0 outside TRAIN, and a role with no overlap writes no file (`strategy_ic_mom_volman_test.cpp`, case "later"). So P9-M's sleeve vol / Sharpe line is an in-sample figure. (2) `--walk-forward` with `--mom-volman` is accepted, and the volman scale schedule then stays the full-TRAIN fit's while the weights walk forward. That is ALCOMB-4's limit, which ALCOMB-4 says the prereg must state for P9-W. | PRE round 2b-ii states both. A report line at the next lane touch. |
| `atx-impl/tests/strategy_ic_runner_test.cpp:874,4855-5097` (19 lines) | Suggested (S-7) | 19 new lines exceed 100 columns. The file already had 543 at BASE; test only. | Reflow at the next touch, or leave as the file's style. |
| `atx-impl/tools/composition_engine_fit.py:709-719` | Suggested (S-8) | `weights_v3` re-implements the runner's lifted-candidate refusal (`strategy_ic_rules.cpp:760-781`). This is a small Python copy of a C++ rule. It needs to exist because the Python adds the `themes` entry, but the two can drift. | Keep it; a comment naming the C++ twin would help. Alternatively, the C++ fit could emit the v3 rows document, making the post-step pure orchestration. |
| `atx-impl/tests/strategy_ic_mom_volman_test.cpp:354` | Suggested (S-9) | The test uses a fixed directory, `temp_directory_path()/"atx_mom_volman_sleeve_files"`. Two IC test runs on the host at once (pools) collide and delete each other's files. | Use a unique directory, like the file's other fixtures. |
| `atx-engine/src/research/composition/fit.cpp:417` | Suggested (S-10) | The core look-ahead inequality (`cut + lag <= j`) has no recorded mutation. The lane's teeth E1-E5 are the refusals. By construction, `NoRowUsesFutureDecisions` turns red on a one-decision look-ahead, because it moves every factor from `cut - 1` on. | Record one mutation (`<= j + 1`) at the next C++ touch. |

## Evidence: my own runs vs the lane's logs

**My runs.** All in pool-23, interpreter `C:/Program Files/Python312/python.exe`, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, own `--basetemp` under my scratch, `git status --porcelain` empty after each.

- `pytest atx-impl/tools/test_fit_walk_forward.py`: **21 passed, 2 skipped, 41 subtests**, exit 0. The skips are `RealExecutables` without exes.
- `pytest test_composition_engine_fit.py test_composition_mom_volman.py test_composition_resid.py test_fit_composition_weights.py` (in `atx-impl/tools`): **155 passed, 1 skipped, 43 subtests**, exit 0.
- `pytest test_fit_walk_forward.py -k RealExecutables` with `ATX_RESEARCH_ADMISSION_EXE` and `ATX_RESEARCH_COMPOSITION_EXE` set to `p9-d3w-h`: **2 passed**, exit 0.
- Gtests: under TEST-1 the C++ verdict rests on the lane's logs and receipts, listed under "From the lane's logs only" below.
  - Before the TEST-1 message reached me, I had run the lane's existing Release exes `p9-d3w-h` (EXE-CUR verified above).
  - I did no configure and no build.
  - These runs corroborate the lane's Release logs `r8-rel-*.log` (17 / 25 / 231, the same counts). The whole-target runs exceed what TEST-1 now allows a reviewer; they are reported, not required.

  | executable | named filter | whole |
  |---|---|---|
  | `atx-engine-research-composition-tests` | `WalkForward.*:MomVolman.*` 7 / 7 | 17 / 17 |
  | `atx-engine-research-eval-tests` | — | 25 / 25 |
  | `atx-impl-strategy-ic-tests` | `WeightsV3.*:CompositionWeightRows.*:MomVolman.*` 11 / 11 | 231 / 231 |

  All exit 0.
- Fingerprints (`d3w-logs/tools/d3w-fingerprints.py`, BASE from `git archive fd084e78 atx-impl/tools atx-engine/tools atx-impl/strategies`):
  - only `composition_engine_fit` (`79966210..` -> `a6dd0da5..`) and `composition_resid` (`d7c911ac..` -> `9c48f390..`) move;
  - `OUTPUT_PRODUCERS (fit, fit_prior)` is `d28a3089..` on both sides, as are the other 9 entries.
- Flag-absent identity (`d3w-logs/tools/ident.py` BASE and HEAD, same WORK dir, then `cmp.py`):
  - 17 files identical;
  - 4 weights files differ only at `provenance.engine_fit.producer_fingerprint` and / or `provenance.resid.producer_fingerprint`. This is exactly the lane's `r8-final-flag-absent-identity.log`.
- Teeth, by my own tool (`rev_teeth.py` in my scratch, TEETH-RESTORE form: it keeps the bytes, restores them in the same run, asserts the restored bytes, then checks the tree):
  - **R-T1** (the lane's M-P1: the `WALK_FORWARD_REFUSED` gate in `check_walk_forward` disabled): **4 failed** (no engine-fit, tsmom, resid, pooled). The control on the restored file: 1 passed, 13 subtests.
  - **R-T2** (the lane's M-P7: strict int / float in `compare_values` disabled): **2 failed** (`test_strict_types_refuse_an_integer_against_an_equal_float`, `test_parent_check_engine_mode_is_strict`). Control: 2 passed.
  - After both, `git status --porcelain` was EMPTY and `git hash-object` equals `HEAD:` for `composition_engine_fit.py` (blob `a3fb49f8...`).
  - The raw-byte SHA-256 of the working file differs from the blob's only by checkout EOL conversion: `core.autocrlf=true`, 804 CRLF, and CRLF->LF equals the blob.
  - The two mutations are the lane's own lines, so the teeth reproduce.

**From the lane's logs only:**
- the C++ verdict of record under TEST-1: the Release gtests `r8-rel-*.log` (17 / 25 / 231) and the receipt `receipt-p9-d3w-h.json`, EXE-CUR all match;
- the Debug gtest rounds `r8-gt-*`;
- the C++ teeth E1-E5, R1-R3 and C1 (`r8-teeth-cpp-*.log`, MUTATED Debug builds);
- the Python teeth M-P2..M-P6 and M-P8;
- the fitter pytest 305 at head;
- `scripts/tests` seed 0 (519 / 5 / 0) and the stage chain (10);
- the eval `--vol` synthetic run.

**Not run by anyone validly:** `scripts/tests` seed 1 (ENV-1 void; see (i)). Deferred to the gate (TEST-1).

**Not run by me, per TEST-1:** the whole `scripts/tests` and the stage chain.

**Blind.** I opened no output dated 2020-2023 or 2024 onward. The only data I read is synthetic test fixtures and logs.

## What root must check at merge

1. **Checks at merge and at the gate (ROOT-1a / TEST-1).**
   - At the merge: a target-scoped Debug build of the touched test targets (`atx-engine-research-composition-tests`, `atx-engine-research-eval-tests`, `atx-impl-strategy-ic-tests`).
   - At the merge, the named filters:
     - `WalkForward.*:MomVolman.*` (composition);
     - `EvalVerb.*` (eval);
     - `WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*` (IC; the last two are the flag-absent neighbours).
   - At the merge: the lane's pytest files and its identity tool (item 3).
   - At the gate (GATE-1): the whole `scripts/tests` under seeds 0 and 1, the stage chain and the whole targets. The lane's seed-1 run is void under ENV-1, so the gate run is the first valid one.
   - D3W changes no `scripts/` code. Its only `scripts/tests` hunk is the D2R-3 S2 assertion, and its Python adds no set-order dependence (`sorted` / `sort_keys` throughout).
2. **B2-EVAL-KEEP.**
   - `eval_report.cpp` is keep-both with B2's PRE-15 hunks; `eval_cli.cpp` / `.hpp` are touched by D3W too.
   - After both are in, run the eval gtests and the PRE-15 gtest.
   - `EvalVerb.VolFlagAddsNetVolOnly` must stay green: it proves the flag-absent document and print.
3. **Identity.** Run the report's procedure: the 3 steps, the same WORK dir, `ident.py` / `cmp.py`.
   - Expected differences are only the two `producer_fingerprint` fields.
   - Note: every `--theme-resid` weights file and every `--engine-fit` weights file produced after the merge differs from a pre-merge one in that field, with no D3W flag set (CM-5 code identity; D2-9 S-c). Any pinned SHA of such a file must be re-derived, never edited to fit.
4. **New IC output files.** Every mom-volman-v1 IC run (no flag; ALCOMB-6) now publishes `<role>_mom_volman_sleeve_raw.csv` and `<role>_mom_volman_sleeve_scaled.csv`.
   - The existing files are byte-identical.
   - A directory digest or file list of such a run differs. I found no strict file-set check in `scripts/` or `atx-impl/tools`.
5. **EXE-CUR and trees.**
   - Pool-23 `build-equity-rel/bin` (`p9-d3w-h`) is current for HEAD's C++.
   - Pool-23 `build-equity/bin` holds teeth-MUTATED Debug exes and is never evidence.
   - Both trees, 4.2 GB in all, are still on disk, contrary to the handoff.
6. **v1 / v2-only readers refuse v3** (fail-closed):
   - `strategy_marginal_ic.cpp:267` (intended: "marginal keeps refusing v3");
   - `atx-impl/tools/book_diagnostics.py:743` (`read_book`);
   - `scripts/research_mine.py:68`.
   - P9-W's cell path must not run them on the v3 file, or PRE defers those lines.
7. **D3 / D3W meeting point.** The fitter flag names, the refused set and the parent fit match D3's `wave_kinds.py:86-90` at pool-20 `3834bc0c`. Re-check at the merge of the second lane.
8. **D3W-STALE.** Prereg round 2b-ii states the staleness of `mom-volman-v1.json:50` beside PRE-40's line.
9. **S-6 into PRE:** the sleeve files are TRAIN-only, and walk-forward with mom-volman keeps the full-TRAIN scale schedule.

## For ABS (does D3W's extension point suffice for `od3p-absent-contract.md` sections 2-5, pool-21?)

The extension point exists where the report says: `strategy_ic_composition.cpp:342` (the member skip in `IcComposition::add`), `:347` (`weight_at`), `add_standardised` `:411` / `:419`. It suffices for the composition semantics. It does not cover the CLI half, which the contract places outside `IcComposition` anyway.

- **Section 4 (semantics): suffices.** A listed member skipped on every date gives the same composite and the same `contribution_fraction` as its signal being non-finite on every cell.
  - In both cases the rank loop finds no finite cell and adds nothing.
  - `eligible_names` comes from the member mask at create (`:293-296`), so neither case changes it.
  - Masses are fixed at create, or, under v3 on standardise, set by the row schedule (`walk_forward_stages`). Both include the absent member's weight, so "mass stays in theme" holds.
- **Simplest route: no `IcComposition` edit at all.** The runner passes `blend_sign` 0 for a listed member at `strategy_ic_runner.cpp:604`. D3W's skip tests `sign == 0` before the dated branch, so the route works with and without v3 rows.
- A per-date skip is not needed: section 4 says "every date". If one ever is, S-3's single `weight_at` makes it one place.
- **Sections 2-3 (list file, flags, `absent-members-v1`, the `absent_members` summary key)** are runner config / CLI work.
  - The capability array is now `strategy_ic_rules.cpp:553`; the contract's `:545-546` moved because of D3W's hunks.
  - The `IcComposition` citations in the contract (`:256-257`, `:263-265`, `:314-318`, `:333`) also moved. Read them at D3W's head.
- **Section 5 (equivalence).** The composite outputs follow from the skip. The listed member's own per-candidate outputs (its IC rows and daily series, `strategy_ic_runner.cpp:585-612`) are computed from its signal. ABS must either blank that signal before scoring, so the equivalence holds over every file, or state the proven output set as section 5 allows.

## Permission refusals

None.
