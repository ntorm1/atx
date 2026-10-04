# Task D2R report: the fitter reads no flag abbreviation (ruling D2R-1)

| | |
|---|---|
| Outcome | DONE |
| Pool / branch | `C:/atx-wt/pool-20`, `feat/p9-d2-20261003` (D2's branch; no switch, no history rewrite) |
| Root head merged | `d7f762cf` (contains `30b9c9d2`), `--no-ff` merge `68715e51`, no conflict |
| Commits | `68715e51` merge, then one code + tests + report commit (this file) |
| Rulings | D2R-1 (defines the lane), E2-9 (the carry it moves from D3), ROOT-1 (root's merge suite), W3-7, D2-9 (read; nothing here changes D3's list except E2-9) |
| Build | none (Python only) |

## The defect, reproduced

After the merge, before any change, `scripts/tests/test_research_spec.py` gave **2 failed, 85 passed**:
`test_e27b_reads_what_the_fitter_and_nav_summ_parse` and `test_the_whole_file_passes_with_a_generated_spec_present`
(the subprocess re-run of the file). The fitter exited with `ambiguous option: --compo could match --composition,
--composition-exe`. The cause: D2's `--composition-exe` (`composition_engine_fit.py:124`) shares every proper prefix of
`--composition`, and argparse's default `allow_abbrev=True` read a unique prefix as its flag. The whole pre-fix
`scripts/tests` run (seed 0) had 3 failed (these two plus the known E2 red), 387 passed, 4 skipped.

## Scan before the change

The fitter builds exactly one parser: `fit_composition_weights.py` `parse_args` (`argparse.ArgumentParser`). The
composed modules (`composition_resid`, the `PARENT_CHECKED_MODIFIERS`, `composition_engine_fit`) only call
`add_argument` on it. There are no subparsers, parent parsers or argument groups. `composition_rules.py:406` has its
own CLI parser (`--out`), which the fitter does not compose. The parser has 33 long options.

Method: every argv was checked token by token (an option token whose text before `=` is not an exact option of the
parser). The spec argvs were also parsed with the parser as built (`allow_abbrev=True`) and with
`allow_abbrev=False`, and the two namespaces were compared. Tools (scratch, not committed): `scan_static.py` (static),
plus a recording hook (`sitecustomize.py` on `PYTHONPATH`, active only with `D2R_SCAN_LOG`). The hook wrapped
`ArgumentParser.parse_known_args` without changing it, and logged every argv that reached a parser with
`--composition` and `--runner-summary-sha256`, in-process and in subprocesses, during the pre-fix test runs.

| Source | Argvs | Relied on an abbreviation | Notes |
|---|---|---|---|
| Committed specs + templates `scripts/specs/**` with a `fit` section, resolved by `RC.load_spec` (templates through their parent chains), with `research_cycle.fit_step`'s full argv (driver flags + `fit.flags` + resid parent / recipe / work dir / max seconds / output) | 67 (42 templates, 25 specs) | **0** | All 67 parse with abbreviations on and off. The namespaces are identical. 0 non-exact tokens. |
| Other committed JSON under `scripts/specs/**` (candidates, rules, waves, blocked, diagnostics, `v9/mine-c1`; not cycle specs) | 27 files, text scan | **0** | Every option token is exact (`--mom-volman`, `--theme-hedge`, `--engine-fit`, `--theme-tsmom`, `--two-speed`) or another tool's flag. |
| Driver argv builders: `research_cycle.fit_step` (`:1576-1588`), the wave modules (`wave_steps`, `wave_stage_cell`, `wave_manifest` rule constants go through `change.flags`, which the template resolution above covers), `research_add_alpha`, `research_mine` | literal flags | **0** | Every fitter flag literal is exact. The dynamically built flags (`research_cycle.py:1508/1511`, `wave_steps.py:72`, `wave_queue.py:286`) go to the fields builder, add-alpha and wave_queue, not the fitter. |
| Committed shell drivers and fixtures that run the fitter (`.superpowers/sdd/mega-alpha-20260926/studies/v4*/v5*/v6*_train.sh`, `v6_w.env.example`, `scripts/tests/fixtures/v61_train_dry.txt`) | 9 commands | **0** | All flags exact (14-16 each). |
| Text scan of every committed `.py/.json/.sh/.txt/.ps1/.toml/.env.example` in `scripts/`, `atx-impl/tools/`, `atx-impl/strategies/`, `atx-engine/tools/`, `.superpowers/` | 1,325 files, 9,753 option tokens; 170 are strict prefixes of a fitter option | **0** outside the e27b test | Triaged by hand. `--out` (117: other tools' flag, incl. `composition_rules`' own CLI), `--admission` (card / monitor), `--theme` (add-alpha / candidates), `--engine` (`c2_gm_pair.py`), `--c` (CSS `var(--c)`), `--a` / `--c` (generic `apply_flags` / `reuse_args` unit tests). Only `test_research_spec.py:876-877` (`--compo`, `--compo=`, `--c`) is fitter-bound: the e27b cases. |
| Dynamic: argvs that reached the fitter's parser during the pre-fix fitter / composition suites (15 files, below) | 347 calls (331 unique; 119 tests; incl. the historical fitter blobs some tests load) | **0** | 0 non-exact tokens |
| Dynamic: same, during the whole pre-fix `scripts/tests` (seed 0) | 16 calls (7 unique; 2 processes) | **only the e27b test** | `--compo` (ambiguous) in `test_e27b_reads_what_the_fitter_and_nav_summ_parse`, in-process and in the whole-file re-run. |

**Result: no pinned or ledgered spec, committed template, fixture, driver or non-e27b test reads a fitter flag through
an abbreviation. Nothing blocks the change.**

`nav_summ` (B-lane, not touched) was checked the same way, for the PM. 66 committed specs with a `summ` section have
0 non-exact `summ.extra` tokens. The driver's literal summ flags (`research_cycle.summ_step`) are exact. The e27b
protocol abbreviations (`--proto`, `--prot=`, `--pr`) still parse uniquely there. So the defect is **not present** in
nav_summ today. The hazard is latent: it still has `allow_abbrev` on (41 long options), so a new flag sharing a prefix
would break or re-route an abbreviation the same way. Listed below; not fixed.

## What changed

- `atx-impl/tools/fit_composition_weights.py:2761-2763` (`parse_args`): `allow_abbrev=False` on the fitter's single
  `ArgumentParser`, plus a two-line comment naming ruling D2R-1. Every flag from every composed module is now read
  only as its exact spelling: two tokens or `--flag=value`. A prefix is now `error: unrecognized arguments: ...`,
  exit 2.
- `atx-impl/tools/test_fit_composition_weights.py:705-762`: new class `ExactFlagsOnly`, 3 tests.
  - `test_an_abbreviated_flag_is_a_usage_error`: each of `--compo X`, `--compo=X`, `--c X`, `--orient prior`,
    `--orient=prior`, `--composition-e c.exe`, `--engine-f`, `--max-sec 5`, `--report-f` and `--h` exits 2 with
    `unrecognized arguments: <tokens>`.
  - `test_no_prefix_of_any_flag_is_read`: spies the real parser, asserts `allow_abbrev` is False, then asserts that
    every proper prefix (3 characters or more) of every long option that is not itself an option exits 2.
  - `test_the_exact_flags_still_parse`: the two-token and `=` forms of `--composition`, `--orientation`,
    `--max-seconds`, plus `--engine-fit` and `--composition-exe`; the defaults are unchanged.
  - Checked against an abbreviation-reading parser (`allow_abbrev` forced on): the first two tests fail and the third
    passes.
- `scripts/tests/test_research_spec.py`: the cross-lane hunks below.

### Cross-lane hunks (E-lane test file `scripts/tests/test_research_spec.py`, listed per ruling D2R-1)

1. `:872-875` comment: the fitter now reads only the exact flag (two tokens or `=`). nav_summ keeps `allow_abbrev`
   on. A former abbreviation is a usage error in the fitter and is still counted by the guard.
2. `:876-881` tables: `COMPOSITION_SPELLINGS` keeps `pair`, `equals`, `repeat-last` and `repeat-first`, which the
   fitter reads. The three former abbreviations (`abbreviation` `--compo X`, `abbreviation-equals` `--compo=X`,
   `shortest` `--c X`) move unchanged into the new `COMPOSITION_ABBREVIATIONS`. No case is deleted.
3. `:917-920` `e27b_cases`: iterates both tables. The plan / run / lock and add-alpha refusal tests still run every
   former case (the guard refuses all of them, as before). Only the order changed: the abbreviations come after the
   repeats.
4. `:923-940` `test_e27b_reads_what_the_fitter_and_nav_summ_parse` (name kept; now takes `capsys`):
   - New: the fitter's parser has `allow_abbrev` False and nav_summ's has it True.
   - The exact spellings parse to `ew-theme-aim-v1` (`ew-theme-v1` for repeat-first), and the guard sees each one
     (unchanged).
   - New: each former abbreviation exits 2 with `unrecognized arguments`, and `RC.argparse_values` still returns
     `[ew-theme-aim-v1]` for it (the safe side).
   - The nav_summ protocol part is unchanged.

The guard itself (`research_cycle.validate_e27b` / `argparse_values`) is not changed. It still counts every prefix,
which is strictly safe.

## Flag-absent identity (D2's synthetic cases)

`identity.py` (scratch) builds D2's `test_composition_engine_fit.World` at a fixed path and runs each of D2's three
`CASES` (`std-erc-tsmom`, `shrink-tsmom`, `std-aim-resid`). The flag is absent, and the run goes through the real
argv path (`fcw.main(argv)`, so the new parser parses it).

| Case | exit pre / post | admission.csv, admission.json, composition_weights.json | stdout summary minus `seconds` and `script_sha256` |
|---|---|---|---|
| std-erc-tsmom | 0 / 0 | byte-identical (3 / 3) | equal |
| shrink-tsmom | 0 / 0 | byte-identical (3 / 3) | equal |
| std-aim-resid | 0 / 0 | byte-identical (3 / 3) | equal |

- "Pre" means the pre-fix fitter: the HEAD blob `c0370f18` in CRLF, whose SHA-256 is the pre-fix `SCRIPT_SHA256`
  `7180751a...`. It ran as a scratch module in front of the same tools dir, and it reproduced a first run made with
  the pre-fix worktree.
- The argv is unchanged.
- `script_sha256` (CM-5: in the receipt, not in the output bytes) moves from `7180751a...` to `8e24943e...`, as any
  fitter edit does.
- `seconds` is wall time.
- `producer_fingerprint` does not move: `parse_args` is outside `OUTPUT_PRODUCERS`' closure.

## Tests (explicit paths, `-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`, `C:/Program Files/Python312/python.exe`)

| Suite (separate invocations) | Seed 0 | Seed 1 |
|---|---|---|
| `scripts/tests` (whole), pre-fix baseline | 387 passed, 4 skipped, **3 failed** (E2 known red + the 2 e27b) | - |
| `scripts/tests` (whole), post-fix | 389 passed, 4 skipped, **1 failed** = `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (root's known red, E2) | 389 passed, 4 skipped, **1 failed** = the same known red |
| Fitter / composition suites (below), post-fix | 284 passed, 2 skipped, 36 subtests | 284 passed, 2 skipped, 36 subtests |
| `scripts/tests/test_no_python_mirror.py` | 7 passed | 7 passed |
| `atx-engine/tools/test_stage_chain.py` (ROOT-1's second suite) | 9 passed | - |

- The fitter / composition files: `atx-impl/tools/test_fit_composition_weights{,_store,_pool}.py`,
  `test_composition_{engine_fit,fit_fixture,ic_shrink,resid,rules,theme_erc,theme_tsmom,two_speed,mom_volman,theme_hedge}.py`
  (D2's list plus the two composition modules root merged since), `test_fit_composition_rule_plugins.py` and
  `test_factor_series_admission.py`.
- Pre-fix, the same set gave 281 passed and 2 skipped. The 3 new tests account for the difference.
- The two skips are the executable-gated tests: `RealExecutables` (needs `ATX_RESEARCH_ADMISSION_EXE` /
  `ATX_RESEARCH_COMPOSITION_EXE`) and B1's exe test. pool-20 has no built executables, and the lane does not build.
- `scripts/tests` skips: `test_cycle_e2e.py:439` (needs `ATX_EQUITY_BIN`) and 3 `RESEARCH_CYCLE_LIVE_ROOT` tests.
- The known red fails the same way in all three runs: the registry-set assertion. Which extra item it shows first
  depends on the seed.
- No red outside the known-red set.

## Evidence (`evidence-D2R/`, beside this report)

- pytest logs:
  - pre-fix: `pre-scripts-tests.log`, `pre-tools-tests.log`;
  - post-fix: `post-scripts-tests-s{0,1}.log`, `post-tools-tests-s{0,1}.log`, `post-mirror-s{0,1}.log`;
  - the stage-chain run was read from the console (9 passed).
- Scan results:
  - static: `scan-static-pre.json`, `scan-static-post.json`;
  - dynamic: `hook-tools-pre-summary.txt`, `hook-scripts-pre-summary.txt`. The raw hook logs, about 680 KB, were not
    kept.
- Identity: `identity-pre2.json`, `identity-post2.json`.
- Tools: `tools/scan_static.py`, `tools/identity.py`, `tools/hook_summary.py` and `tools/d2r_scan_hook.py`. The hook is
  named `sitecustomize.py` in a dir on `PYTHONPATH` when used, and is inert without `D2R_SCAN_LOG`.

## What root re-runs at merge

1. Whole `scripts/tests` under one seed (ROOT-1). Expected: the two e27b tests green, and only
   `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` red until E2's merge.
2. `atx-engine/tools/test_stage_chain.py`, as a separate invocation.
3. The fitter / composition suites above, with `ATX_RESEARCH_ADMISSION_EXE` / `ATX_RESEARCH_COMPOSITION_EXE` /
   `ATX_EQUITY_TARGETS_EXE` set from root's build, so `RealExecutables` and B1's exe test run. Their argvs use only
   exact flags (dynamic scan: 0 non-exact tokens), so no change is expected. The lane could not run them.
4. `scripts/tests/test_no_python_mirror.py`.

No C++ changed: no build and no gtest.

## Notes and open questions for the PM

- **Stale text in the E-lane guard (not edited: not a listed hunk).** These now overstate what the fitter reads; the
  guard behaviour is still safe (it counts every prefix):
  - `research_cycle.py:442-445`, the `argparse_values` docstring: "argparse (`allow_abbrev` on, its default)".
  - `:466-470`, the `validate_e27b` docstring: "whatever the spelling of --composition in fit.flags (`=`, an
    abbreviation, a repeat ...)".
  - `:482-484`, the refusal text: "argparse also reads '--composition=VALUE', an abbreviation or a repeat".
  
  All three are true for nav_summ only. A text-only follow-up for E2 / P10 if the PM wants it.
- **nav_summ (B-lane) keeps `allow_abbrev` on.** The scan shows no present defect: 0 committed argvs use an
  abbreviation, and its e27b protocol prefixes parse uniquely. But the hazard is the same as the fitter's: the next
  flag sharing a prefix with `--protocol` (or any other flag) silently breaks or re-routes it. Listed for the PM, not
  fixed.
- **Environment.** `python` on PATH resolves to `C:\atx\atx-db\.venv\Scripts\python.exe`. The first reproduction run
  (`test_research_spec.py`, `PYTHONDONTWRITEBYTECODE=1`) and the first static scan used that interpreter, read-only:
  nothing was installed or written there, and nothing under `atx-db/` was edited. Every run counted above used
  `C:/Program Files/Python312/python.exe` explicitly, the interpreter D2's suites name, which has `psutil`.
