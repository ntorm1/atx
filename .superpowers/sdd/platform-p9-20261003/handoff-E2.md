# Handoff: lane E2 (P9), fix round 3, stopped by the owner

- Pool: `C:/atx-wt/pool-17`. Branch: `feat/p9-e2-20261003`. Lane base: `1239a5ff`.
- Code head: `daaa483c` (`wip(e2): fix round 3 ...`). This file is committed on top of it; see `git log -1`.
- History, all on the branch, nothing squashed:
  - Round 1 (`05a0fcd1`) and round 2 (`d0054b32`) are complete and reported in `task-E2-report.md`
    ("## Fix round 1", "## Fix round 2").
  - Round 2 was re-reviewed in `task-E2-rereview-2.md` (`558070e5`): CHANGES_REQUIRED (R2c + S1-S3).
- Round 3 status: the code and tests for every item are committed in `daaa483c`. NOT done:
  - the full suites after these changes;
  - the "## Fix round 3" report section;
  - the final `fix(e2): ...` commit.
- No process of this lane is running. The round-3 suite run was stopped minutes after its start, at the owner stop
  (both PIDs killed, nothing else of mine runs).

## Rulings that bind round 3
All are in pool-2 `progress.md`:
- **E2-8:** one raw-rule-flag guard on the resolved flag lists, every v2 kind.
- **E2-9:** N4 fixed; N3 one line; N5 is a P10 note; `allow_abbrev` goes to D3.
- **E2-10:** `nav.leverage` is a rule parameter. Only kind leverage (range-checked against the NAV table, never above
  2.0, OD-P9-3) or the gross-match stage (L') sets it. Any other kind that sets or changes it, by any route, is
  refused at preflight. The three Suggested items are fixed in the same round.
- **E2-11:** SPO: nothing to do. `--risk-model` stays reserved; SPO rows are an optional C3 carry. The report needs
  one sentence on this.
- **E2-12:** the unmapped-IC-rule example must be an id no lane can register.

## State per item of round 3 (all in `daaa483c`)
| item | state | where | test (all in `scripts/tests/`) |
|---|---|---|---|
| E2-10 `nav.leverage` (R2c) | DONE | `wave_stage_kinds.leverage_refusal` (called from `rule_flag_guard` after the cell is resolved). A raw set / unset key `nav.leverage` in any kind is refused, and so is a resolved cell whose `nav.leverage` differs from the parent's (from `leverage.value` for kind leverage); the message names key, value and route. `preflight_kind` (leverage): refused when the NAV table lists no fixed-v1 range, and above `wave_kinds.LEVERAGE_MAX` = 2.0. `raw_routes` now returns `{route: {key: value}}` | `test_wave_kinds.py::test_nav_leverage_is_set_only_by_a_leverage_wave` (7 probes: 5.0 on horizon / rule / role, gross none and pm6-6, whole `nav` section, `change.unset`, raw key in a leverage wave); `::test_leverage_wave_cap_and_table_and_the_matched_l_prime` (1.5 passes, 2.5 refused by the cap on a table allowing 3, no fixed-v1 refused, a pm6-6 horizon cell publishes its L' in the `-gm` copy) |
| S1 N3 test with teeth | DONE | `test_wave_driver.py::test_cache_path_bound_w1_i1`, new last block: the parent gets its own shorter `ic.cache "c"` (not on the shared stores), and the bound is set between the two depths | mutation checked: with N3's line reverted to `cell = spec` the test fails `assert 0 == 3`; the line was restored (no diff) |
| S2 case / single-dash spellings | DONE | `wave_kinds.norm_option` (single `-` -> `--`, option name case-folded). Used by `abbreviated`, `raw_rule_flags`, `wave_stage_kinds.option_tokens` and `occurrences` (argparse_values on normalised lists) | 3 rows added to `E2_8_PROBES` in `test_wave_kinds.py` (`--Vol-Target` by set, `-theme-erc` by flag map, `-theme-erc` by set) |
| S3 route message | DONE | `wave_stage_kinds.touching` names a flag-map entry whose VALUE is the token ("`rule_cell.constants.flags nav --cadence --vol-target`") | 1 row in `E2_8_PROBES` |
| E2-12 unmapped example | DONE | `test_wave_kinds.py` `UNMAPPED = "zz-unmapped-rule-v0"`, used in `test_rule_flags_from_the_tables` (plus `assert UNMAPPED not in WK.COMPOSITION_FLAGS`) and in `test_raw_rule_flag_guard_allows_free_flags_and_names_unmapped_ic_rules`; no `theme-cov-hedge` left in scripts/ | not re-run after this edit (literal swap) |
| E2-11 | report sentence only | - | - |

### Mutation check
`wave_kinds.py` and `wave_stage_kinds.py` were stashed back to `d0054b32`. The 12 new round-3 tests failed (4 spelling /
route probes, 7 leverage probes, the cap test), each preflight passing. The stash was popped clean.

## Tests run after the round-3 code
- `test_wave_kinds.py` (whole file) + `test_wave_driver.py -k cache_path_bound`, PYTHONHASHSEED=0: 75 passed. This ran
  BEFORE the E2-12 edit.
- Mutation runs as above.
- NOT run:
  - `scripts/tests` full under seeds 0 and 1;
  - `atx-engine/tools/test_stage_chain.py` under seeds 0 and 1;
  - `test_wave_kinds.py` after the E2-12 edit.
- Expected after round 3: scripts/tests 446 + 12 = 458 passed, 4 skipped (the 4 live-input gates). The test_wave_kinds
  count goes 62 -> 74. The test_wave_driver count is unchanged (28; the W1-I1 test was extended, not added).

## Next steps (in order)
1. Re-run the edited file first (about 75 s), with a basetemp of your own in the session scratchpad:
   ```
   PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=0 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2f3-k scripts/tests/test_wave_kinds.py
   ```
   Expect 74 passed.
2. Full suites, SEPARATE invocations, one after the other (about 16 min per seed), each with its own `--basetemp`:
   ```
   PYTHONHASHSEED=0 ... -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2f3-s0 scripts/tests
   PYTHONHASHSEED=0 ... -m pytest -q -p no:cacheprovider -rfEs --basetemp=<scratch>/e2f3-c0 atx-engine/tools/test_stage_chain.py
   ```
   Then the same pair with `PYTHONHASHSEED=1` (`e2f3-s1`, `e2f3-c1`). Log to `.superpowers/sdd/platform-p9-20261003/logs/`.
   A PowerShell `*>>` log is UTF-16: normalise it to UTF-8 after. A red is fixed, never by editing an expected value.
3. Append "## Fix round 3" to `task-E2-report.md`, same shape as round 2:
   - the table above;
   - evidence (commands, exit codes, tails), the mutation checks;
   - E2-11's sentence: no v2 wave hands an SPO cell a new risk model in P9; `--risk-model` stays reserved to the
     table's rules, and SPO rows are an optional C3 carry;
   - E2-12;
   - decisions:
     - the explicit `nav.leverage` check replaces the reviewer's `{leverage}` substitution idea (leverage reaches the
       argv only through `{leverage}`, so the key check covers every route with a clearer message);
     - library kinds copy the parent's nav (add-alpha), so they have no route to it;
     - a raw `nav.leverage` in a leverage wave is refused because `leverage.value` would override it silently.
4. Commit as `fix(e2): ...` on top (squash nothing). `git add -f` the report. Trailer exactly
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
5. Delete your own scratch by name. This session's round-3 scratch was already deleted at the stop.

## Traps
- `rtk` rewrites git / grep output: use `rtk proxy <cmd>` or the Grep tool.
- Never run `scripts/tests` and any `atx-engine/tools` test in one pytest session (conftest window bind: 31 false
  reds).
- A combined PowerShell command that writes a file with .NET and then runs `git checkout -- file` was denied by the
  permission system. Use the Edit tool or `git stash push -- <files>` / `git stash pop` for mutation checks.
- Failed-stage messages go to stderr; tests read them from `receipts/NN-<stage>.failed-1.json` ["error"].
- Blind rule: synthetic fake worlds only; nothing dated 2024 or later is opened, and no 2020-2023 output is read.
- v1 plan bytes must stay unchanged. The guard and the plan lines are v2 only (`kind` is None for v1);
  `test_verify_plan_line_states_the_rule_cell_refusal` asserts no `--list-rules` / E2-8 line in a v1 plan.
