# Task A-3 report: research_cycle plumbing (lane A, pool-11)

Branch `feat/platform-v8-a-20260929`, base `ef11f462`. No C++, no real data, nothing built. Python only.

## What was built

Files: `scripts/research_cycle.py` (modified), `scripts/run_bounded_research.py` (modified), `scripts/research_tree.py`
(new, 90 lines: shared git / no-git / window helpers), `scripts/research_ledger.py` (new, 120 lines: trial-ledger reader
and the protocol line), `scripts/cycle_verdict.py` (new, 100 lines: cycle_verdict.json, used by A-2's `--screen` and by
every full run), `scripts/tests/test_research_cycle.py` (tests added, three filters adapted), `scripts/specs/v71.json`
(one key, see cross-lane edits).

Every addition is off unless the spec or the command line asks for it; the v6.1 / v7.0 / v7.1 plan lines are unchanged
(the fixture identity tests `test_plan_equals_v61_train_sh_dry_output_pin_for_pin`, `test_bounded_lines_equal_the_
executed_v61_receipts`, `test_v70_plan_dry_run_resolves_every_pin`, `test_v71_plan_dry_run_resolves_every_pin_and_the_
identity_phases` pass unmodified apart from the `"marginal"` phase name in two order filters).

Interfaces as coded:

- **Stage inputs (L9).** `INPUT_KEYS` += `sec_identity_bridge`, `earnings_calendar`, `insider`, `sec_filings`,
  `thirteenf`, `ftd`, `regsho_threshold`, `security_master`, `short_volume_ext`, `reuse_fields`. `Cycle.stage_flags()`
  appends, after `--sic-events` and before `--fund-lag-sessions`: `--sec-stages ROOT --sec-identity-bridge DIR
  --sec-identity-bridge-sha256 PIN --earnings-calendar-sha256 PIN --insider-sha256 PIN --sec-filings-sha256 PIN`, then
  `--<key> DIR --<key>-sha256 PIN` for each holdings stage present (table `HOLDINGS_INPUTS`). `reuse_fields` gives
  `--reuse DIR --reuse-sha256 PIN` after the caps, plus `--reuse-hardlink` when `fields.reuse_hardlink`. Validation: the
  four SEC inputs come together; the three stage dirs are named `earnings_calendar/`, `insider/`, `sec_filings/` under one
  parent (that parent is `--sec-stages`); `reuse_fields` needs a built fields section; CLI `--reuse-fields` is refused
  when the spec pins `reuse_fields`.
- **Clean check by pathspec.** `research_tree.CODE_PATHSPEC = (atx-core, atx-tsdb, atx-engine, atx-impl, scripts,
  CMakeLists.txt, CMakePresets.json, cmake)`; `research_tree.dirty_paths(root) -> (blocking, ignored)`. `run_cycle`
  checks before every executed process phase (not only at start); paths outside the pathspec are logged once
  (`# dirty outside the code pathspec (listed, not a stop): ...`). `run_bounded_research.py` uses the same rule and
  writes `dirty_outside_pathspec` (and `git`) into every receipt.
- **`summ.dsr_n: "ledger+1"`.** Needs `summ.ledger`, excludes `summ.cells`; resolved when the summ step is built (steps
  are re-resolved after every phase, so a cell ledgered between plan and run is counted). Integer `dsr_n` keeps the v7
  stop-and-name rule.
- **Protocol lines (PM addition 1).** `research_ledger.cells()` is the one ledger reader of research_cycle
  (`ledger_cells`, hence `cells_from_ledger` and `"ledger+1"`); it skips kind `protocol`. `research_ledger.protocol_line
  (window_id, owner_ruling, date, research_window_sha256)` builds `{schema atx.trial-ledger/v1, kind protocol, count 0,
  window_id, owner_ruling, date, research_window_sha256, trial_id}` (no cell; trial_id = sha256 of [protocol, window id,
  window sha, date], 16 hex); `append()` writes it with backtest_integrity's encoding (sorted keys, compact) and never
  twice. Verb: `research_cycle.py ledger-protocol --ledger build-equity/trials.jsonl --owner-ruling "..." --date
  2026-09-29` (window id defaults to W0-1's, window file to `atx-impl/strategies/research_window.json`). The
  atx.trial-ledger/v1 ledger has no hash-chain field (only the C++ trial_ledger.hpp chains lines): the protocol line is
  kept append-only like every line; `count` 0 keeps nav_summ's Appendix A total unchanged and its series readers skip
  lines without `series`.
- **`build: "equity" | "equity-rel"`.** Table `BUILDS` (bin dir + DLL dirs) and `EXE_NAMES`: a missing exe defaults to
  the build's bin dir, a bare name goes in it, a path is kept; `env_path_prepend` defaults to the build's DLL dirs.
- **Derived stores.** When the spec omits `ic.cache` / `fit.work_dir`:
  `<out_root or build-equity>/candidate-cache/<role sha16>-<window id>` and `.../fit-work/<role sha16>-<window id>`
  (C-1's layout), never suffixed (shared, content-keyed). The window id comes from W0-1 (`research_window.WINDOW_ID`,
  else derived from `atx.research-window/v2` -> `research-window-v2`); if W0-1 is absent the derivation stops with
  exit 2 instead of guessing.
- **`out_root`.** A root-relative dir every relative output name is placed under (fields, ref, u, w, fit, card,
  marginal, nav, monitor); as-built pinned fields dirs are not placed. Absolute out_roots are refused (the bounded runner
  writes only inside its root).
- **Ledger copy.** `summ.ledger_copy: PATH` copies the ledger there after a successful summ (log line with its SHA).
- **Per-phase runner caps (OD-2).** Spec `runner.phases: {phase: {seconds, max_rss_mib, min_free_mib}}`. When a phase is
  not named there, the data table `RUNNER_PHASE_RULES` applies: u and w get 300 s / 2,560 MiB when the role manifest's
  `dates` > 1,200; everything else keeps the runner caps (180 s / 1,536 MiB in every spec). `--runner-override` wins
  over both. A hash-only root (no role file) keeps the base caps.
- **`receipts: "every-phase"`.** fields, check, monitor and summ run through the bounded runner too (run dirs
  `<fields>-run`, `<monitor>-run`, and fresh `<cycle dir>/{check,summ}-run<k>` for the always-run phases).
- **`ref` skipped** when this cycle's fields manifest SHA equals the `baseline_fields` pin (the parent's fields); its
  compare steps are skipped with it.
- **K3 `--no-git`.** `research_cycle.py ... --no-git` and `run_bounded_research.py --root R --no-git` are accepted only
  when R is outside any git repository (`research_tree.repo_root_of` walks up for `.git`); otherwise exit 2. With it:
  no clean check, the runner argv gains `--root R --no-git`, the receipt has `source_sha: null` and `git: "none ..."`,
  and a relative tool path (runner, builder, static check, fit, card, monitor, summ) absent under R resolves to this
  worktree's copy (so a tmp root needs only its data and a spec).
- `run_bounded_research.py --root R` (default: its worktree) is where the child runs and where outputs must live.
- Also in this commit (used by A-2, tested there): phase `marginal`, `--screen`, `exe_capabilities`, `cycle_verdict.py`
  (every run now writes `<out_root or build-equity>/cycle-<name>[-suffix]/cycle_verdict.json`; an extra file, no
  accepted output changes).

## How root verifies

```bash
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_cycle.py
```

Result on this branch: 82 passed, 3 skipped (the three `RESEARCH_CYCLE_LIVE_ROOT` tests). Base `ef11f462` had 64 passed
and 2 failed (`test_v71_spec_is_v70...`, `test_v71_plan_dry_run...`: v71.json said dsr_n 37, the tests 35); both pass now.

New tests (the brief's seven, the PM's `test_protocol_line_not_counted_in_dsr_n`, and five that pin behaviour): `test_stage_inputs_map_to_flags` (the ten L9 inputs, exact
flag order, the L9 pins), `test_stage_inputs_are_validated` (5 cases), `test_clean_check_pathspec` (a real git work tree:
a tracked sprint-dir file modified and a new report written during U do not stop the cycle; an `atx-impl/` edit during
U stops it before fit), `test_dsr_n_from_ledger`, `test_every_phase_has_receipt`, `test_ref_skipped_when_fields_
unchanged`, `test_no_git_only_outside_repo` (research_cycle and a real `run_bounded_research.py` subprocess),
`test_build_key_resolves_exe_dir`, `test_runner_phase_caps_follow_the_od2_rule_and_the_spec`,
`test_out_root_places_outputs_and_derives_shared_stores`, `test_ledger_copied_after_summ`,
`test_protocol_line_not_counted_in_dsr_n` (the verb appends once; N and the grid skip the line).

Identity runs for root (brief step 3), unchanged: `research_cycle.py plan scripts/specs/v71.json --suffix p8` then `run`
with the same argv; the ten daily and events files must equal the v7.1 cell's. The v71 summ line now reads
`--dsr-n <ledger lines + 1>` from the ledger instead of the hand-set 37 (same value when the ledger has 36 lines).

## Deviations from the brief (with reasons)

1. **OD-2 condition on role dates, not scored sessions.** The dispatch said "more than 1,200 scored sessions". The 3-year
   role has 1,155 dates (about 756 scored), the 4-year role about 1,405 dates (about 1,006 scored): on scored sessions the
   cap would never fire on the 4-year role, which is what OD-2 is for (plan: "1,481 of 1,536 MiB at 1,155 dates; the
   4-year role has about 1,405 dates"). The rule is data: `RUNNER_PHASE_RULES` accepts `role_dates_over` and
   `scored_sessions_over`; root can switch the key in one line or pin caps per spec with `runner.phases`.
2. **Every-phase receipts are opt-in** (`"receipts": "every-phase"`), because turning the direct phases into bounded ones
   changes the v6.1 / v7.0 / v7.1 plan lines that the fixture identity tests pin. Specs written by `add-alpha` set it.
3. **The ledger copy is a spec key** (`summ.ledger_copy`), since the sprint directory is not known to the code.
4. **Derived stores need W0-1.** W0-1 has not reached this branch; with it absent a spec that omits `ic.cache` /
   `fit.work_dir` stops (exit 2, "cannot be derived"). Tests monkeypatch the window id. Root: nothing to do after the
   W0-1 merge. The cache root is new, so the first cycle on it is cold unless root seeds it once, e.g.
   `cp -al build-equity/mega-candidate-cache-v71/. build-equity/candidate-cache/<role16>-research-window-v2/`.
5. **No `role` stage (PM addition 2).** Role building stays outside the cycle: a role stage needs the base role, the
   identity bridge, the SIC events and (lo3) the delisting inputs as named pins plus the builder's own caps, and no
   brief test covers it. Root builds roles by hand (W0-2) and the spec pins the result as `inputs.role`.
6. **`cache gc` (PM addition 3)** comes after A-2, as the PM ordered; it is reported with A-2.

## Cross-lane edits

- `scripts/specs/v71.json`: `summ.dsr_n` 37 -> `"ledger+1"` and the matching description sentence. Value-preserving: the
  resolved N equals the integer whenever the integer was correct (otherwise the v7 rule stopped the cycle).
- For lane E (E-3, contract K3): `research_cycle.py run tiny.json --root <tmp> --no-git` works with a spec whose tool
  paths are the repo-relative ones (`scripts/run_bounded_research.py`, `atx-impl/tools/...`): they resolve to this
  worktree's copies when absent under the tmp root. Exes must be absolute (ATX_EQUITY_BIN) or under the root.

## Open risks

- `git status` pathspec entries are relative to the root; root must be the worktree top (it is by default).
- A spec with `out_root` puts outputs in an untracked dir of the worktree (listed as dirty outside the pathspec, never a
  stop); add it to `.gitignore` if root wants a quiet `git status`.
- The full test file takes about 3 minutes (subprocess-heavy fake tools on Windows).
