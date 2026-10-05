# Task D2R review: light review of the fitter's `allow_abbrev=False` (ruling D2R-1)

| | |
|---|---|
| Verdict | **APPROVE**. 0 Required, 2 Suggested. |
| Lane / pool / branch | D2R, `C:/atx-wt/pool-20`, `feat/p9-d2-20261003` |
| Scope | `68715e51..283043ce`: fix `faa30011`, evidence `283043ce`. `68715e51` is the merge of root `d7f762cf`. |
| Reviewer | A fresh, independent light reviewer. I edited no code, ran no build and opened no real data. I read root history only as one-line subjects. |
| Rulings checked | D2R-1, D2R-2, ROOT-1, ENV-1 (no 0xC0000142 exit in any of my runs) |

## Spec verdict (D2R-1, item by item): PASS

| D2R-1 item | Result | Evidence |
|---|---|---|
| The lane merges root's head first | PASS | `68715e51` has parents `c55063ff` and `d7f762cf`. `git merge-tree --write-tree c55063ff d7f762cf` gives `ca61c2f9`, which is also `68715e51^{tree}`. `git diff d7f762cf 68715e51` is empty. So this is a plain merge with no hand edits, and its tree equals root's. |
| `allow_abbrev=False` on the fitter's argparse | PASS | `fit_composition_weights.py:2763` is the only parser the fitter builds. There are no subparsers, parent parsers or argument groups. The 9 composed modules only call `add_argument(parser)`. `composition_rules.py:406` is a separate CLI that the fitter does not compose. I spied on the real parser through `fcw.main([])`: 1 parser is built, `allow_abbrev` is False, and it has 33 long options. Nothing else about the parser changed: `description` is the same, there is no `add_help`, `exit_on_error` or `prefix_chars` change, and no flag was added, removed or re-defaulted. |
| e27b tables and assertions state the new truth | PASS | See (b) below. |
| The refusal is asserted against the fitter itself | PASS | Both tests call the real `fcw.parse_args`, or `tool_parser(fcw.main)`, which spies on the real `ArgumentParser.parse_args`. Neither test uses a copy of the parser. |
| Scan before the change; stop if a pinned or ledgered spec relies on an abbreviation | PASS | I redid the scan independently with my own method; see (c). No spec was edited. |
| The cross-lane edit is named | PASS | The `scripts/tests/test_research_spec.py` hunks are listed in `task-D2R-report.md` §"Cross-lane hunks". |

## Quality verdict: PASS (2 suggested tightenings)

**(a) Parser.** The fix is the one-line `allow_abbrev=False` plus a 2-line comment. I compared `code_fingerprint` for every producer tuple of `68715e51` and `283043ce`: OUTPUT, FACTOR, CONTEXT, AIM and AIM_ERA. All five are unchanged, so `producer_fingerprint` does not move. The working file's SHA-256 is `8e24943e...`, which matches the lane's stated post-fix `SCRIPT_SHA256`. That hash goes in the stdout receipt only (CM-5). No committed file outside the git-ignored sprint dirs pins `7180751a` or `8e24943e`.

**(b) e27b edits are not weakened.**
- No case was deleted. The three former abbreviations (`--compo X`, `--compo=X`, `--c X`) moved unchanged into `COMPOSITION_ABBREVIATIONS`.
- `e27b_cases` still feeds every former case to the plan / run / lock and add-alpha guard tests, which still assert refusal and that nothing was written.
- For each abbreviation, the old `fcw.parse_args(...).composition == V5_AIM` became two checks: `SystemExit` code 2 with `unrecognized arguments`, and `argparse_values(...) == [V5_AIM]`. The second is stronger than the old `V5_AIM in ...`.
- New: the fitter's parser has `allow_abbrev` False and nav_summ's has it True.
- The exact spellings keep their old assertions.
- The nav_summ protocol part is unchanged.

**(c) Independent scan. Result: 0 fitter-bound abbreviations.** I did not use the lane's tool. My method:
1. Take the 33 long options from the real parser.
2. Tokenize every committed text file in the tree except `atx-db/`: 5,363 files and 72,436 `--x` tokens. Any token (or the part before `=`) that is a strict prefix of a fitter option and not itself an option counts as a hit. That gave 2,239 hits.
3. Walk every string list in all 1,016 committed JSON files.
4. Intersect the hits with the 258 files that name the fitter, `fcw`, `fit_step` or `FIT_REQUIRED`, and with every test file. Then triage by hand, checking every hit that is a *unique* prefix (one that argparse would have read) against its context.

What I found:
- **`scripts/specs/**`**: 0 hits in any JSON flag list. The only text hit is `v7u-lo3.json:13` `--out`, inside a `prepare_recent_research.py` `role_command`.
- **Driver argv builders**: all fitter literals are exact.
  - `research_cycle.fit_step` (`:1549-1589`)
  - `research_roles.py:198/242/245` (`--era`, `--era-id`)
  - `wave_steps.rule_cell_doc`, which only merges `constants.flags` into `change.flags`. Those come from committed JSON, which is covered above.
  - The dynamic `f"--{key}"` builders: `research_cycle.py:1508/1511` (`stage_flags`, which goes to the fields builder), `wave_steps.py:72` (add-alpha) and `wave_queue.py:286`. None of them reach the fitter.
  - The `--admission` hits at `research_cycle.py:1522/1530` are the card and monitor CLIs.
- **Shell drivers and fixtures**: `v6u_train.sh:49` and `v6_w.env.example:46` `--out` belong to `prepare_recent_research.py`. `v61_train_dry.txt` and `v61_receipt_commands.json` have 0 hits.
- **`atx-impl/tools/**`, `atx-engine/tools/**`, tests**:
  - `--out`: `composition_rules`' own CLI, the engine tools and the C++ CLIs.
  - `--admission`: `alpha_report_card` and `book_monitor`.
  - `--theme`: add-alpha.
  - `--a` / `--c`: generic placeholders in the `apply_flags` and `reuse_args` unit tests, which no parser reads.
  - The only fitter-bound abbreviations are the lane's own refusal cases: `test_fit_composition_weights.py:705-762` and `test_research_spec.py:880-881`.

So no pinned or ledgered spec was edited, and none relies on an abbreviation. This agrees with the lane's account.

**(d) Nothing pinned moved.** `git diff --stat 68715e51 283043ce` touches exactly the files the report names:
- `fit_composition_weights.py` (+3 / -1)
- `test_fit_composition_weights.py` (+58, additive only)
- `test_research_spec.py` (the e27b block only)
- `task-D2R-report.md` and `evidence-D2R/**` (sprint dir)

No expected hash, golden, digest or pinned value is in the diff. In `identity-pre2.json` and `identity-post2.json` only `script_sha256` and the module path differ. The three D2 cases' outputs are equal.

**(e) Teeth (mutation M1, reverted).** I removed `allow_abbrev=False` from `fit_composition_weights.py:2763` and ran the tests:

| Test | Result under M1 | Expected |
|---|---|---|
| `test_an_abbreviated_flag_is_a_usage_error` | FAIL: `--compo` gives "ambiguous option", not "unrecognized arguments" | FAIL |
| `test_no_prefix_of_any_flag_is_read` | FAIL: `assertFalse(parser.allow_abbrev)` fails | FAIL |
| `test_the_exact_flags_still_parse` | pass | pass: it is a no-regression test |
| `test_e27b_reads_what_the_fitter_and_nav_summ_parse` | FAIL: `allow_abbrev` assert fails | FAIL |
| `test_e27b_refuses_every_spelling_at_plan_run_and_lock` and `..._at_add_alpha` | pass | pass: they test the guard, which did not change |

I restored the file. Its SHA-256 is `8e24943e...` again, `git status --porcelain` is empty, and `git diff --quiet` is clean. After the revert, ExactFlagsOnly gave 3 passed and the e27b test 1 passed.

A second mutation was refused by the permission system. It would have deleted the e27b test's `allow_abbrev` assert to show that its abbreviation loop also has teeth on its own. I did not retry it or route around it (see "Permission refusal"). So the loop's own teeth are unproven by mutation. Reading M1's stderr, though, the abbreviating parser reports "ambiguous option" for `--compo`. The loop's `"unrecognized arguments"` check would therefore fail on it.

**(f) Tests.** All runs used explicit paths, `-p no:cacheprovider`, my own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`, `PYTHONHASHSEED=0` and `C:/Program Files/Python312/python.exe`.

| Suite | Result |
|---|---|
| `scripts/tests/test_research_spec.py` | **87 passed**, exit 0 (323 s). Both former known reds are green. |
| The 15 fitter / composition files the report names (`test_fit_composition_weights{,_store,_pool}`, `test_composition_{engine_fit,fit_fixture,ic_shrink,resid,rules,theme_erc,theme_tsmom,two_speed,mom_volman,theme_hedge}`, `test_fit_composition_rule_plugins`, `test_factor_series_admission`), one invocation | **284 passed, 2 skipped, 36 subtests**, exit 0. The skips are `test_composition_engine_fit.py:668` and `test_factor_series_admission.py:481`, which need built executables. The counts match the lane's. |

**(g) Lane notes.** Under D2R-2, the stale text at `research_cycle.py:442-484` and nav_summ still reading abbreviations are ruled, not findings.

**Also checked, no finding:**
- Two added lines are 121 columns. No line length is configured for these directories, and both test files already had 15 and 30 lines over 120 before this change.
- `unittest.mock` is imported (`test_fit_composition_weights.py:16`).
- Nothing in `evidence-D2R/` is real data or a TRAIN statistic. The identity cases are D2's synthetic `World`.

## Required

None.

## Suggested

| # | path:line | Problem | Fix |
|---|---|---|---|
| S1 | `atx-impl/tools/test_fit_composition_weights.py:750` | The per-prefix loop checks only exit code 2. With abbreviations on, most prefixes also exit 2: they are ambiguous (`--c`, `--orient`); an invalid choice (`--s x`); an invalid float (`--max-sec x`); or a stray `x` after a store_true flag (`--engine-f x`). So the loop catches only prefixes of free-string value flags, such as `--work` and `--library-s`. The test's real teeth are the `allow_abbrev` assert at `:745`. | For each prefix, also require `f"unrecognized arguments: {p} x"` in the stderr that `exit_code` already returns, as `test_an_abbreviated_flag_is_a_usage_error` does. |
| S2 | `scripts/tests/test_research_spec.py:938` | The abbreviation loop checks for `"unrecognized arguments"` without the token, so another unrecognized token in the argv would also satisfy it. | Assert `f"unrecognized arguments: {' '.join(sp)}"` (ExactFlagsOnly already passes with this exact form). |

Neither suggestion blocks the merge. If wanted, they can be done in the next lane that edits these files.

## Permission refusal

The permission system refused an `Edit` of `C:/atx-wt/pool-20/scripts/tests/test_research_spec.py`. The edit would have deleted line 930 (`assert not tool_parser(fcw.main).allow_abbrev and tool_parser(NS.main).allow_abbrev`) as a temporary mutation, to be reverted. The classifier's reason was "Security Test Removal". I did not retry it or route around it. The file was never changed: its SHA-256 `3ab2369d...` equals the blob at `283043ce`.

## What root must check at merge

1. Merge `283043ce`. `68715e51`'s tree equals root `d7f762cf`'s, so only `faa30011` and `283043ce` bring changes. Root's sprint dir has no `task-D2R-*` or `evidence-D2R/` today, so no add/add conflict is expected unless root writes them first.
2. ROOT-1: run the whole `scripts/tests` under one seed. Expected: both e27b tests are green. The only red should be `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry`, and only if E2's merge has not cleared it yet. Run `atx-engine/tools/test_stage_chain.py` as a separate invocation.
3. Re-run the fitter / composition suites with `ATX_RESEARCH_ADMISSION_EXE`, `ATX_RESEARCH_COMPOSITION_EXE` and `ATX_EQUITY_TARGETS_EXE` set from root's build. The two exe-gated skips I saw (`test_composition_engine_fit.py:668`, `test_factor_series_admission.py:481`) must then run green. Neither the lane nor this review could run them.
4. Any lane that adds or renames a fitter flag after this merge needs no e27b table change. A merged lane that *uses* a fitter abbreviation will now fail with exit 2. Root should check that the lanes merged after D2R-1 (such as C3 and OD3P) pass exact flags only. Re-running the whole `scripts/tests` covers their committed specs.
5. D2R-2 (b): when B2's post-identity redo sets nav_summ `allow_abbrev=False`, it must flip the `tool_parser(NS.main).allow_abbrev` half of `test_research_spec.py:930`. It must also update the `PROTOCOL_SPELLINGS` cases the same way D2R did for `--composition`. Until then, that assert deliberately pins nav_summ's present state.
