# Lane OD3P report

## Outcome

DONE_WITH_CONCERNS -- every dispatch item done except T2 (lane item 3), which is PENDING because B2's redo is not in
root (r6-wave3-common / resume7-addendum rule); the clean-source line also needs the C++ `absent-members-v1` lane
(OD3P-1); the classes guard is red on this lane's two round-1 literals until root adds their rows (SQL2-CLS).

## Branch / SHA

`feat/p9-od3p-20261004` @ HEAD (see the final commit of this round; all work committed, tree clean), pool-21.

## Frozen base / lease

base `5c57c1ef` (lease run id `p9-od3p-20261004`; leases not touched). Root merges: `9c7aa654` (`09e2350a`, round 1),
`4dc831ac` (`e809271f`, RESUME 7 step 1), and the final merge of this round (section "Root merges").

## Required items (dispatch R7-1, OD3P steps 1-7) -> change -> test -> evidence

| # | item | change (file:line at HEAD) | tests | evidence |
|---|---|---|---|---|
| 1 | merge root, re-run the two files | merge `e809271f` (root `4dc831ac`), no conflict, none of my files changed | `atx-impl/tools/test_od3_tooling.py` + `scripts/tests/test_research_cycle_od3.py`: 53 passed | `od3p-evidence/r7-step1-merged-e809271f.log` |
| 2 | OD3P-1 (replaces the pinned 0) | `atx-impl/tools/carry_weights.py:24-38` (doc), `:154-164` `check_drop` (theme_standardise refusal removed), `:195-205` (no zeroing; `carry.json` gains the `drop` record); `scripts/research_roles.py:29-37` (doc), `:82-84` constants, `:342-344` no-history refusal, `:389-411` `RolesCycle.absent_members`, `:441` the call | tool: `test_drop_carries_train_bytes_and_only_records_the_list` (ew-theme-std-v1, theme-erc-v1, ic-shrink-v1), `test_drop_refusals_publish_nothing` (12 cases); cycle: `test_drop_passes_the_absent_list_to_the_history_ic_passes_only`, `test_drop_runs_the_real_carry_and_the_carried_weights_are_train_bytes`, `test_drop_is_refused_when_the_ic_exe_lacks_the_capability` (injected, real probe of a fake `.cmd` exe without / with the capability, a non-exe), `test_drop_with_no_history_role_is_refused`, `test_drop_changes_only_the_history_ic_passes_against_the_pinned_zero_module`, `test_drop_refuses_a_spec_that_applies_a_flagged_rule` | `od3p-evidence/r7-item2-own-tests.log` (61 passed); teeth `od3p-evidence/teeth-r7.log` (script `mutate-r7.py`) |
| 2 | contract for the C++ lane | `od3p-absent-contract.md` (list-file schema and keys, flag spelling, capability, semantics, the defining equivalence, refusals, where the cycle puts the flags, what the C++ lane hands back) | - | - |
| 3 | ALCLOCK-2 | `scripts/specs/p9/od3/flagged_members.json`: `info-clock-rate-v1` into `rules` with `rule_source_sample_end` 2019; `rules_pending` = [`mv-aim-v1`]; description / ruling text for OD3P-1 | `test_the_committed_list_is_the_final_flag_list_of_its_source` (also: the list as committed is refused, resolved it carries the book with no weight changed) | in the item-2 log |
| 4 | runbook (lane item 6) | `p9-od3-runbook.md` | - | - |
| 5 | suites (lane item 7) | - | see "Tests" | `od3p-evidence/r7-*.log` |
| 6 | T2 (lane item 3) | PENDING: B2's redo is not in root (`git -C C:/atx-wt/pool-2 log --format=%s -30` shows no B2 merge) | - | - |
| 7 | report, handoff | this file, `handoff-OD3P.md` | - | - |

### OD3P-1 as implemented

- **Carry tool.** `--drop-list L --drop-list-sha256 S` checks the list and the base and records the list in
  `carry.json` (`drop`: rule `drop-flagged-members-v1`, `list_sha256`, `source_sha256`, `members` sorted, `rules`
  sorted). It changes no weight: every weights file is the bytes of the same carry without the list (the anchor file is
  W's bytes; each era file is W rebound with `carried_from`, no `drop` block). Kept refusals (exit 1, nothing
  published): an id not a key of W's `weights`, a repeated or malformed id, an empty list, pending rules, bad schema /
  source, a pin mismatch, a base applying a listed rule, a non-v1/v2 weights document, no `--era`. Removed: the
  `theme_standardise` refusal (theme-erc-v1 and ic-shrink bases are accepted; tested).
- **Cycle.** `fit.carry.drop {path, sha256}`: the u and w IC passes of every history role (`is_history`) get
  `--absent-members <path> --absent-members-sha256 <pin>` appended to the IC exe's argv, and the runner gets
  `--bind <path>` as its last bind before `--`; TRAIN roles, NAV, fields and summ get nothing; the carry gets
  `--drop-list`. Refused (exit 2, plan / status / run, before any step): the IC exe's `--list-rules --json` envelope
  does not offer `absent-members-v1` (message names the capability, the history roles, OD3P-1 and "not read: no
  producer"); a drop with no history role (new: the list would apply nowhere); a nav / ref flag or nav rule naming a
  listed rule (kept).
- Design choice to note: the flags go on both IC passes of a history role (u and w), so the exe sees the member absent
  in every pass of that role (the ruling's "on history roles only"); the u-pass outputs of a carry cycle feed nothing
  (the carry replaces the pooled fit). The contract leaves `--no-composition` + the flag to the C++ lane (apply or
  refuse); the OD-3 read never sends the pair.

## Teeth (every refusal and every new behaviour; working-tree mutation, test run, file restored byte-identical)

22 mutations, all CAUGHT, all restored byte-identical (`od3p-evidence/teeth-r7.log`; sha256 of the three files checked
after the run): R1 capability refusal, R2 no-history refusal, R3 history roles only, R4 the flags reach the steps, R5 the
runner binds the list, R6 the u pass gets the flags, R7 / R7b the carry zeroes no weight (tool test and cycle run), R8
the carry record, R9 no `--era`, R10 theme-erc / ic-shrink accepted; and M1-M3, M5, M6, M9-M14 of round 1 re-run on the
new code (M4, M7, M8 tested code OD3P-1 removed).

## Tests

All with `C:/Program Files/Python312/python.exe`, explicit paths, `-p no:cacheprovider`, own `--basetemp`,
`PYTHONDONTWRITEBYTECODE=1`; `scripts/tests` and the stage chain in separate invocations. No ENV-1 exit seen.

| suite | tree | result | log (`od3p-evidence/`) |
|---|---|---|---|
| lane files (`scripts/tests/test_research_cycle_od3.py`, `atx-impl/tools/test_od3_tooling.py`) | `e809271f` | 53 passed | `r7-step1-merged-e809271f.log` |
| lane files, after OD3P-1 | `9be598cf` code | 61 passed | `r7-item2-own-tests.log` |
| lane files | `e9bedcc7` | 61 passed | `r7-e9bedcc7-lane-files.log` |
| `scripts/tests/test_no_python_mirror.py` | `e9bedcc7` | 7 passed | `r7-e9bedcc7-no-python-mirror.log` |
| whole `scripts/tests`, `PYTHONHASHSEED=0` | `e9bedcc7` | 543 passed, 4 skipped | `r7-e9bedcc7-scripts-tests-seed0.log` |
| whole `scripts/tests`, `PYTHONHASHSEED=1` | `e9bedcc7` | 543 passed, 4 skipped | `r7-e9bedcc7-scripts-tests-seed1.log` |
| `atx-engine/tools/test_stage_chain.py` | `e9bedcc7` | 9 passed | `r7-e9bedcc7-stage-chain.log` |
| `atx-engine/tools/test_research_store_classes.py` | `e9bedcc7` | 1 failed (expected: this lane's two literals, SQL2-CLS), 4 passed, 96 subtests passed | `r7-e9bedcc7-classes-guard.log` |
| teeth (22 mutations) | `9be598cf` code | 22 caught, files restored byte-identical | `teeth-r7.log` |
| lane files (R8 round, after merging root `1b64ce45`, docs only) | `70439eef` | 61 passed | `r8-70439eef-lane-files.log` |

## New literals, flags, keys, rule ids (for SQL2-CLS / the prereg)

- Schema `atx.od3-flagged-members/v1` (the list; round 1); record schema `atx.carry-weights/v1` (the branch tool's).
- Rule ids: `carry-train-weights-v1` (branch), `drop-flagged-members-v1` (the carry record's drop rule).
- Capability `absent-members-v1` (offered by the IC exe once the C++ lane lands; read by the cycle).
- Flags: IC exe `--absent-members`, `--absent-members-sha256` (C++ lane); carry tool `--drop-list`,
  `--drop-list-sha256`.
- Spec keys: `fit.carry {weights, admission}`, `fit.carry.drop {path, sha256}`, `roles[].label_dir`,
  `roles[].label_manifest_sha256`.
- JSON keys: `carry.json` `drop` (this tool); IC `summary.json` `absent_members` (contract, C++ lane).
- No CMake tail append; no C++ in this lane.
- **Classes guard (SQL2-CLS; as SQL3-1).** `atx-engine/tools/test_research_store_classes.py` in this lane flags the two
  literals of `atx-impl/tools/carry_weights.py` (present since round 1) besides root's expected `atx.nav-spec/v1`.
  Proposed rows for root to add at the merge: `atx.carry-weights/v1` -> class `fit-output` `schemas` (`carry.json`,
  published into `fit.output` beside `composition_weights.json`); `atx.od3-flagged-members/v1` -> class `cycle-spec`
  `schemas` (a spec-side input under `scripts/specs/p9/od3/`, pinned by `fit.carry.drop`). On `e9bedcc7` (root's
  `atx.nav-spec/v1` row now in) the guard flags exactly these two literals: 1 failed
  (`test_every_schema_literal_is_registered`), 4 passed, 96 subtests passed
  (`od3p-evidence/r7-e9bedcc7-classes-guard.log`). The lane does not edit `classes.json` (one owner, as SQL3-1): an
  expected red of this lane until root adds the two rows at the merge.

## Cross-lane edits

- `scripts/cycle/cli.py` `make_cycle`: the 3-line T1 refusal (round 1, E2's file; unchanged this round).
- No edit to the W3-8a kind files, to B2's files (`nav_summ`, `cycle_verdict.py`, `cycle_resume.py`,
  `wave_stage_record.py`, `scripts/cycle/spec.py`) or to the IC runner (D3W's).
- D2R-2 (a) (the stale fitter-abbreviation text) not done: it sits in `scripts/cycle/spec.py`, B2's file this session.

## For PRE (Appendix IV item 32: "F.1 era fields argv from P9-F0h's fields manifest - lane OD3P's runbook")

Proposed text (runbook section 2, B7): the era fields argv is the `command` list of P9-F0h's TRAIN fields receipt
`start.json` (for v15: `build-equity/train-2020-2023-lo3-fields-v15-run/start.json`) with exactly `--role`,
`--role-sha256` and `--output` replaced by the era's values and `--reuse`, `--reuse-sha256`, `--reuse-hardlink`
removed; the `-c` program, `--fields` (84 names, in the receipt's order), every source and pin, `--mgr13f-stage` and the
child caps unchanged; the runner keeps the receipt's limits and bindings except the role binding (the era's), the reuse
manifest binding (removed) and its run dir. Self-check with the TRAIN values first; any other diff is a stop; a run
refused at the 600 s cap is a stop (the PM rules the staging). P9-F0h's receipt is [UNKNOWN until the last accepted
cell] (v15 unless a cell added a field).

## For root (the identity procedure at the merge)

1. Merge; re-run `atx-impl/tools/test_od3_tooling.py` and `scripts/tests/test_research_cycle_od3.py` (explicit paths,
   `-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`), then the whole `scripts/tests` under
   `PYTHONHASHSEED` 0 and 1 and `atx-engine/tools/test_stage_chain.py` separately.
2. Flag-absent identity, proven by tests on fakes (no exe, no data): (a) without `fit.carry` / `label_dir` / a
   `--risk-model` history book, plan, status, every step argv and `lock` equal `research_roles.py` blob `313b23b3`
   (root's file before the lane) on four specs: `test_without_the_new_keys_plans_equal_the_integration_8_module`;
   (b) a `fit.carry` spec without `drop`: plan, status and every step argv equal the round-1 module (blob `8081a6d0`,
   commit `937865f0`), and with `drop` the only differences are the OD3P-1 tokens on the history roles' u / w argv and
   the carry note: `test_drop_changes_only_the_history_ic_passes_against_the_pinned_zero_module`; (c)
   `carry_weights.py` without `--drop-list`: every published byte, the printed file list and the refusal text equal the
   branch tool (blob `186655d9`, sha256 `5aebb3a7...`): `test_without_a_drop_list_every_byte_is_the_branch_tools`.
   Substitution list: none (no exe, no golden, no pinned value moved; no build).
3. No real-data run belongs to this lane. After the C++ lane lands: `<frozen ic exe> --list-rules --json` lists
   `absent-members-v1`, and `research_cycle.py plan` of a `fit.carry.drop` spec plans with the E1 / E2 u / w argv
   ending in the two flags (the probe path is tested here on a fake exe only).
4. Before the read (runbook P0): resolve `mv-aim-v1` in `flagged_members.json` from COV-MV's registration, re-pin.

## Root merges

| merge | root head | conflicts |
|---|---|---|
| `09e2350a` (round 1) | `9c7aa654` | none |
| `e809271f` (step 1) | `4dc831ac` | none (no lane file changed by root) |
| `e9bedcc7` (last step; suites below run on it) | `ebc918c3` (SQL2-CLS: `atx.nav-spec/v1` registered) | none (no lane file changed by root) |
| `70439eef` (R8 round, after the DIRTY-1 commit `52805177`) | `1b64ce45` (sprint-dir docs and logs only) | none (no file outside the sprint dir changed); lane files re-run: 61 passed |

A seed-0 run started on `05e2a373` was stopped (my own process, confirmed gone) when root moved, and its partial log
deleted; every suite figure below is on `e9bedcc7`.

## Deviations from the dispatch

- T2 pending (B2 not in root), as the dispatch rule says.
- Tests and teeth were run with `C:/Program Files/Python312/python.exe` explicitly: `python` on this host's PATH
  resolves to `C:\atx\atx-db\.venv\Scripts\python.exe` (the step-1 re-run used it once, read-only, writing nothing;
  every later run uses the system interpreter).

## Open questions (one line each)

1. OD3P-1 placement: the flags go on the u and the w IC passes of a history role; if the PM wants the w pass only,
   `ABSENT_PHASES` in `research_roles.py` is the one-line change (tests iterate it).
2. The contract's equivalence covers every output file; if the C++ lane can prove only the composite outputs (IC-result
   cache), the PM rules on its narrower set (contract section 5).

## Ledger candidates

- `python` on this host's PATH is the atx-db venv interpreter (`C:\atx\atx-db\.venv\Scripts\python.exe`): lanes call
  `C:/Program Files/Python312/python.exe` explicitly.

## Fix round 1 (review `91fae138` of `d78d23ce`: CHANGES_REQUIRED, R-1 + S-1 .. S-6; dispatch "OD3P fix round 1")

Outcome: DONE_WITH_CONCERNS (OWNER STOP relayed by the PM during the suites: the whole `scripts/tests` under PYTHONHASHSEED=1 was started and stopped by me, unfinished, as it could not end within 10 minutes; every other suite ran). R-1 closed; S-2 and S-4 (required by the PM) closed; S-1, S-3, S-5 done (small); S-6 not done
(root's: `classes.json` untouched); T2 still PENDING (B2's redo is not in root: `git -C C:/atx-wt/pool-2 log
--format=%s -30` shows no B2 merge). Commits: fix `365b8c8b`; merge of root `fd084e78` = `97a8157f` (plain: its tree
`32ab8e74` equals `git merge-tree --write-tree 365b8c8b fd084e78`; no conflict; root's range touched no lane file:
SQL3's merge and its `classes.json` row); then the records commit (this report, the handoff, the suite logs).

### Findings -> change -> test -> teeth

| finding | change (file:line at the records commit) | test | evidence (`od3p-evidence/`) |
|---|---|---|---|
| **R-1** (Required) | Runbook B6 `fit` bullet `p9-od3-runbook.md:126-142`: `fit.output` REPLACED by the book's own fresh `build-equity/od3-fit-<book>` (`p9f0h`, `yf0`, `r2`, `clean`); never the TRAIN fit dir, never another line's (lines 2 and 4 distinct even when both carry Y-F0's TRAIN fit); the dir, its `-run1` and `.pending` absent before P7. P7 `:152-162`: STOP unless the fit phase is `pending` with the carry argv's `--output` that dir (line 4: `... --output build-equity/od3-fit-clean --drop-list <list> --drop-list-sha256 <pin>`). Stop list `:227-228`. Code (fail closed as well): `scripts/research_roles.py:363` call, `:380-408` `RolesCycle.check_carry_output`: a `fit.output` that exists is refused at plan / status / run (exit 2, before any step) unless its `carry.json` (`atx.carry-weights/v1`) records exactly this carry: W's and A's SHA-256, the anchor role, the eras, and the drop list's pin (or no drop). | `test_each_history_book_gets_its_own_fresh_fit_output_and_the_carry_runs` (the fixed B6 on the fakes: lines 2 and 4 of one TRAIN fit plan `fit` pending with `--output` their own dir, line 4's carry argv ending in `--drop-list`; both run through the real carry; line 4's `carry.json` records the list, line 2's none; the weights files are the same bytes; line 4's history w pass ends in the absent flags; a re-plan shows `fit` done = resume); `test_a_fit_output_that_is_not_this_carrys_is_refused_at_plan` (5 cases: the TRAIN fit dir holding W = the reviewer's case, an existing empty dir, line 2's dir for line 4, line 4's dir for line 2, another W's carry in the dir; each exit 2, nothing runs) | Reproduced FIRST on the lane's fakes, before any change: `fix1-r1-probe-before.log` (old B6: `# phase fit [bounded; done; ...]`, then `HARD-STOP [w:E1]: an upstream pin is unresolved` after `fields F-E1, F-E2, check, U-E1, U-E2`; no era file, no `carry.json`. Shared dir: line 4's fit `done` from line 2, its `carry.json` without the drop record). After: `fix1-r1-probe-after.log` (old B6 and the shared dir refused at plan, exit 2, nothing run; the fixed B6: both lines plan `fit` pending, run through the carry, status `done`). Script `fix1-probe-r1.py`. Teeth F1, F2, F3 |
| S-1 | `research_roles.py:193-199` `carry_tool` (lazy import of `atx-impl/tools/carry_weights.py`, as `scripts/wave_readers.py` imports `nav_summ`; no copied logic), `:362` call, `:365-378` `check_drop_list`: `carry_weights.drop_list` and `check_drop` at plan time (exit 2). Variant of the review's second half: "refuse a carry whose fit.output already exists" would also refuse every resume after the carry ran (`test_carry_replaces_the_pooled_fit_and_feeds_every_era` asserts `fit` done); R-1's guard refuses an existing `fit.output` unless it is this carry's own output. | `test_the_carry_tools_list_checks_run_at_plan_time` (pending rules, unknown id, flagged rule in the base, bad schema; each exit 2 at plan, no step run, no `out/`) | teeth F4 |
| S-2 (PM: required) | Test only. `scripts/tests/test_research_cycle_od3.py:555-589` `test_the_absent_flags_reach_only_the_history_roles_ic_passes`, two shapes: E1 history / E2 inside TRAIN, not the anchor / E3 anchor; and E1, E2 history / E3 inside TRAIN, not the anchor / E4 anchor. Asserts: the set of steps carrying `--absent-members` is exactly {u, w} x the history roles; those end in the two flags with the list bound last before `--`; every other step's argv equals the same spec's without `fit.carry.drop` (the carry adds `--drop-list`); the non-anchor TRAIN role's u / w argv hold neither flag nor the list. `drop_root` gains `eras` (`:237-251`; default = the old two eras, so every earlier test is unchanged). | the test | teeth F5 (the review's probe P1, `history = list(self.eras[:-1])`, on the new test), F5b (P1 on the whole lane file, `-k "drop or absent"`: now 2 failed, where the review saw it pass), F6 (`history = <history>[:1]`, caught by the two-history shape) |
| S-3 | `research_roles.py:470-474`: a history u / w pass whose argv has no bounded-runner `--` raises `CycleError` (exit 2) instead of being skipped silently. | `test_a_history_ic_pass_without_the_runner_is_refused_not_skipped` (u and w each stripped of the runner: refused; today's shape: both get the flags) | teeth F7 |
| S-4 (PM: required) | `p9-od3-runbook.md:215-217`: "P9-F0h's history S2 net Sharpe = its TRAIN S2 net Sharpe + P9-W's dSR (PRE-39; prereg B.6 as PRE-39 reads it), and "lower than on 2020-2023" (PM7-28's registered expectation, carried)". Root's prereg F.1 line still reads "P9-F0's"; the ledger ruling PRE-39 wins; not this lane's file. | - (doc) | - |
| S-5 | "Blind" below restates the round-1 disclosure. | - | - |
| S-6 | Not done: root's (SQL2-CLS; root picks the class whose ingest treats the list as a pinned input). | - | - |

Also: `od3p-absent-contract.md` section 6 (cycle side) lists the three new plan-time refusals in one added bullet;
sections 2-5 (lane ABS's spec) are unchanged. The module doc of `research_roles.py` (`:29-44`) states them too.

Teeth (`od3p-evidence/teeth-fix1.log`, script `mutate-fix1.py`): working-tree mutations of `scripts/research_roles.py`,
each run against the named test, the file restored and its SHA-256 checked equal, `git status` clean after. F1 the
R-1 guard call removed; F2 the guard ignoring the drop pin; F3 the guard refusing its own output (resume); F4 the S-1
call removed; F5 / F5b probe P1; F6 first history era only; F7 the silent skip restored. 8 of 8 CAUGHT, all restored
byte-identical (sha256 prefix `1a70e29cadf5`).

### Tests (fix round 1)

All with `C:/Program Files/Python312/python.exe`, explicit paths, `-p no:cacheprovider`, own `--basetemp`,
`PYTHONDONTWRITEBYTECODE=1`; separate invocations. No ENV-1 exit seen; no permission refusal.

| suite | tree | result | log (`od3p-evidence/`) |
|---|---|---|---|
| `scripts/tests/test_research_cycle_od3.py` alone, after the change | `91fae138` + the fix (uncommitted) | 44 passed | not logged (the first run) |
| lane files (`scripts/tests/test_research_cycle_od3.py`, `atx-impl/tools/test_od3_tooling.py`) | `97a8157f` | 74 passed (61 + 13 new) | `fix1-97a8157f-lane-files.log` |
| `scripts/tests/test_no_python_mirror.py` | `97a8157f` | 7 passed | `fix1-97a8157f-no-python-mirror.log` |
| `atx-engine/tools/test_research_store_classes.py` | `97a8157f` | 1 failed, 4 passed, 96 subtests passed: the expected SQL2-CLS red, naming exactly `atx.carry-weights/v1` and `atx.od3-flagged-members/v1` in `atx-impl/tools/carry_weights.py` (no new literal this round) | `fix1-97a8157f-classes-guard.log` |
| `atx-engine/tools/test_stage_chain.py` | `97a8157f` | 10 passed | `fix1-97a8157f-stage-chain.log` |
| whole `scripts/tests`, `PYTHONHASHSEED=0` | `97a8157f` | 563 passed, 5 skipped (2374.5 s on a loaded host) | `fix1-97a8157f-scripts-tests-seed0.log` |
| whole `scripts/tests`, `PYTHONHASHSEED=1` | `97a8157f` | NOT RUN to the end: started after seed 0, stopped by me at the OWNER STOP (seed 0 took 39.6 min); its partial log deleted. Next step: re-run it | `fix1-97a8157f-scripts-tests-seed1.log` |
| teeth (8 mutations) | `365b8c8b` code | 8 caught, restored byte-identical | `teeth-fix1.log` |

### Blind (review S-5)

- Restated from the round-1 handoff (`80484f0b:.superpowers/sdd/platform-p9-20261003/handoff-OD3P.md`, its "Records"
  item), which the R7 handoff overwrote: the committed record `v8-od3-protocol.md`, read in round 1 as that dispatch
  required, quotes two v8 TRAIN figures of R-2 (in two of its sections); none was used. A one-line root subject seen
  in `git log --format=%s` carried Q5 q-values; none was used.
- This round: that handoff passage was read with every digit masked; root history was read as one-line subjects only
  (none carried a statistic); for the PRE-39 wording I read `p9-prereg.md` (root's) lines 498-504, 905-915 and
  1199-1203, which include F.1's registered ERC prediction ranges (from the literature review; the runbook prints that
  prediction "not read"); not used. No 2020-2023 return, IC, Sharpe, turnover or NAV figure was read or written; no
  data, no exe, no build; `holdout_gate.py` never run.

### For root (fix round 1 additions to the identity procedure)

- Flag-absent identity is unchanged: the new checks run only with `fit.carry` (the list checks only with
  `fit.carry.drop`); `test_without_the_new_keys_plans_equal_the_integration_8_module` (blob `313b23b3`) and
  `test_drop_changes_only_the_history_ic_passes_against_the_pinned_zero_module` (blob `8081a6d0`) pass unchanged;
  `carry_weights.py` is unchanged this round (its blob identity test against `186655d9` passes).
- `research_roles.py` imports `atx-impl/tools/carry_weights.py` lazily (only for a `fit.carry` spec). Root re-runs the
  lane files, then the whole `scripts/tests` (seeds 0, 1) and `test_stage_chain.py` separately.
- Re-review checks: B6 names a fresh `build-equity/od3-fit-<book>` per book (lines 2 and 4 distinct); P7 stops unless
  `fit` is `pending` with that `--output`; the cycle refuses a reused dir at plan time.

### Deviations (fix round 1)

- S-1's "refuse a carry whose fit.output already exists" is done as "refuse an existing fit.output that is not this
  carry's own output" (keeps resume; see the table).
- The merge `97a8157f` carries git's default message without the trailer (as `70439eef` did); not amended (no history
  rewrite).

### Open questions (fix round 1, one line each)

1. PM: is the S-1 variant (an existing `fit.output` accepted only when its `carry.json` records this exact carry) the
   intended reading, rather than refusing any existing `fit.output` (which would also refuse resume)?
2. Root's prereg F.1 prediction line still names P9-F0 where PRE-39 names P9-F0h: a ROUND 2b text item for root.
