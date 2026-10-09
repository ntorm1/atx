# Review: lane AL-CLOCK (first review)

**Verdict: APPROVE.** 0 Required, 5 Suggested.

- Lane / pool: AL-CLOCK, `C:/atx-wt/pool-30`, branch `feat/p9-alclock2-20261007` (TEETH-POOL; pool-8 untouched).
- HEAD reviewed: `7ebd41ff` (records); code head `1cbe5233`. BASE `685e238b` = `git merge-base 7ebd41ff
  feat/platform-p9-20261003` (REV-BASE).
- Range `685e238b..7ebd41ff`, outside `.superpowers`: 21 files, +1669 / -32 (16 C++ files, the CMake tail, the new
  gtest file, the registration, the template, the pytest). `git diff 1cbe5233 7ebd41ff` outside `.superpowers` is
  empty, so the records commits change no code.
- Merges in the range: `adf1c6cb`, `e40ed78c`, `c3f3f1f8`, `8a6ebe5b`, `ccc96e89`. For each one,
  `git merge-tree --write-tree <parents>` gives the commit's own tree, so all five are plain merges.
- Contract: `r7-review-dispatch.md`, `resume8-addendum.md`, `resume9-addendum.md` (TEST-1, TEETH-RESTORE, HOST-3),
  `r8-review-notes.md` "AL-CLOCK", `r7`/`r8`/`r9-dispatch.md` "AL-CLOCK", `briefs/brief-AL-CLOCK.md`,
  `wave3-carry.md` (AL-CLOCK, "brief-AL-CLOCK.md", All lanes), and the ledger rulings ALCLOCK-1/-2/-4/-T2, W3-9,
  W3-16, C3-5, C3-13, PRE-7a, DEC-16, TEETH-POOL/-RESTORE, EXE-CUR, SPDLOG-RELINK, PERM-1, REV-BASE, TEST-1.
  ALCLOCK-3 has no ledger "Ruling" line; I took its text from `status-8.md:157-158`. COVMV-1 is at
  `status-8.md:159-162`.

## Spec verdict (item by item)

### `r7-dispatch.md` "AL-CLOCK"

| # | item | verdict | where |
|---|---|---|---|
| 1 | merge root | done | `c3f3f1f8` (root `1b64ce45`), `e40ed78c` (`fd084e78`), `adf1c6cb` (`685e238b`); all plain |
| 2a | trade-rate row `info-clock-rate-v1` plus one parser entry, append-only (W3-9) | done | `nav_rules.cpp:75-82` is the last row of `trade_rates[]`; `info_clock_refuses` `:57-58` is a new array; `nav_cli.cpp:206` is a new `--rate` value; `nav_spec.cpp:124` is the table's last key |
| 2b | `two_speed_fast_theta(cadence)` (W3-16) | done | `nav_decide.cpp:10-12` calls `bk::two_speed_fast_theta`; the recipe (`nav_write.cpp:254`) and the block (`:299`) use the same function; no literal .12945 in code. `KernelAtTheWindowBoundary` checks it bit for bit at C = 1, 2, 5, 21 |
| 2c | refusals | done | config `nav_validate.cpp:84-88`; input geometry, rule-without-clock and domain `:185-196`; `--fields` `nav_load.cpp:209-212`; grid cadence `nav_lockstep.cpp:203-210`; kernel `nav_decide.cpp:19-30`; duplicate switch `nav_cli.cpp:51-54`. Teeth set 1 caught 7 / 7 (from the lane's logs) |
| 2d | per-book stats | done | `count_info_clock` `nav_decide.cpp:103-118`: members of a main book's scored effective rebalance rows, index order; capacity books excluded via `scored` |
| 3 | ALCLOCK-1 print-only opt-in | done | `--info-clock-print` / NavSpec `info_clock_print`; the print loads the clock without writing to `fields_used` (`nav_load.cpp:159-166`, `ClockLoad::PrintOnly`); the block goes into the main summary only (`nav_publish.cpp:80-82`). Tests: `FlagAbsentLoadsNoClock`, `PrintOnlyOptInAddsOneBlockPerScenario` (non-rule books v5 fixed and v6 + cost-v2 + capacity: every file except `summary.json` is identical, and `summary.json` minus the blocks equals the flag-absent file), `RuleOnWithoutFlagPrintsNoBlock`, `SpecEqualsArgv` (three shapes) |
| 4 | 1-vs-4 `--book-workers` with the rule on; E2 `rule_flags` preflight on fakes; NAV argv token and how a kind-rule wave resolves | done | `BookWorkersOneAndFourSameBytes` (rule + cost-v2 + capacity + print); `test_info_clock_rule.py` (5 tests). Token is `--rate info-clock-rate-v1` on the append route; the kind-rule cell's argv carries `--rule aim-partial-v5` once (from `nav.rule`) and `--rate` once (fake world, `Cycle.nav_step`). The real-exe probe is root's at merge (C3-5) |
| 5 | registration + template | done | `scripts/specs/p9/rules/info-clock-rate-v1.json` (SHA-256 `51b8b754...`, LF, matches the report); `scripts/specs/p9/templates/info-clock.json` (`95d4104a...` = the registration's `template_sha256`). Template has AL-COMB's key set, no `rules`, no `change.flags`. `gross_match pm6-6-engine` is in the manifest fragment, as AL-COMB's are. Ids: rule 18 chars, template name 13, wave 14 (W1-I1) |
| 6 | Debug then Release builds, named filters, whole target-tests, canary on both goldens | partly done; the rest pending by rule | Debug `p9-alclock-h`: named 24 / 24 and canary 15 / 15 (lane logs). Release, whole target-tests and canaries are deferred to the gate (TEST-1) |
| 7 | report with "For PRE" P9-C block | done | `task-AL-CLOCK-report.md` "For PRE": rule, row, both SHA-256s, parent conditions, producers of every printed figure, the flag pair, C3-4, DEC-16 |

### `r8-dispatch.md` and `r9-dispatch.md` "AL-CLOCK"

| item | verdict |
|---|---|
| compile the WIP; C++ tests plus CMake tail (`atx-impl/tests/CMakeLists.txt:210-214`, a tail append) | done (`p9-alclock-b`, then `-e`, `-h`; my `-j` build has 0 warning lines) |
| R9 step 1: records copied from frozen pool-8 and committed (`3cc8a356`) | done; pool-8 untouched by me |
| R9 step 3: teeth set 2 under TEETH-RESTORE | the lane skipped it, as the rule required; ALCLOCK-T2 gave it to the reviewer, and I did it (below): **caught** |
| R9 step 4: merge root `685e238b` | done (`adf1c6cb`, docs only) |
| R9 step 4: Debug `p9-alclock-h` (named, canary) | done (lane logs; EXE-CUR checked below) |
| R9 step 4: Release `p9-alclock-i`, whole suites | pending by rule (TEST-1, deferred to the gate) |
| object trees deleted by name | done by the lane; I deleted my own `p9-alclock-j` tree (below) |

### Rulings and carry items

| ruling / carry | verdict |
|---|---|
| ALCLOCK-1 | done (item 3). Flag absent changes no code path: the new loads, the reserve term, the budget term (`budget.add(0, 8)` is a no-op, `nav_replay_internal.hpp:120-123`), the census and the block are all gated on `rate == InfoClockV1`, the print flag, or a non-empty clock. The `rate_ok` refactor (`nav_validate.cpp:92-93`) gives the old Fixed predicate unchanged |
| ALCLOCK-2 | done. `source_sample_end` 2019, `source_sample_flag` "FLAGGED under PRE-19 / PRE-31" (`:12-14`). The mapping to `ea_days_since` is `fixed_choices[0]` (`:41`) and matches the field's definition (`atx-engine/tools/research_fields_sec.py:160-163`) |
| ALCLOCK-3 | done. `strategy_nav_v7.cpp:1342-1343`: the capacity curve accepts only `info-clock-rate-v1`; aim-partial-v6 still refuses every non-fixed rate. Test `CapacityCurveAcceptsThisRateOnly`; teeth both ways (set 1 M10 by the lane, set 2 by me); the registration states it (`fixed_choices[5]`, `:46`) |
| ALCLOCK-4 | the bound is correct and stated. `realised_leverage` (`:51`) gives planned gross <= (theta_f / theta_s) x gross(aim). I checked the derivation: G' <= (1 - theta_s) G + theta_f A is a contraction toward theta_f A / theta_s, and 2.589 = .12945 / .05. One sentence there is narrower than the ruling: see S-2 |
| W3-9 | done: one row plus one parser entry, appended. COV-MV keep-both is root's (below) |
| W3-16 | done |
| C3-5 / C3-13 | the token is stated and the route is append (not a `--rule` row), shown on fakes; the real-exe probe is root's (below) |
| PRE-7a (analogue) | the opt-in is print-only, not a rule flag, and named alike on the cell and its reference (registration `print_opt_in`, template text) |
| DEC-16 | `restatement` (`:52`) and the report give the name-day vs theme argument; no new constant (theta_f is Y-5's function, theta_s is Y-5's .05, the window is [29]'s 21 sessions) |
| K-P9-7 row shape (`x-flag`, no params, `incompatible[]`) | done. The row agrees with the code: per-name-v1 is the same `--rate`; two-speed-v1 is refused at `nav_validate.cpp:107`; aim-partial-v6 and spo-v1..v3 are refused at `strategy_nav_v7.cpp:1292` / `:1342` (and at runtime `:664`) |
| K-P9-11 registration keys | done; a superset of AL-COMB's key set, with `nav_flag` in place of `fitter_flag` |
| ALCOMB-1 template | done |
| W1-MIN M-1 (1 vs 4 workers, rule on) | done |
| W1-I1 | done (18 chars) |
| SQL2-CLS | the lane adds no `atx.<name>/v<n>` literal (grep of the added lines). The registration's `atx.p9-rule-registration/v1` and the template's `atx.research-cycle-template/v1` already exist |
| CMake tail (P2) | done |
| No Python mirror of the C++ rule (G-P5) | done; the pytest carries only a fake listing of the row (see S-4) |
| Brief gtest names | `StepChangeClosedForm` and `EventClockIsCausal` exist. The brief's `.FlagAbsentIdentity` is `InfoClockPrint.FlagAbsentLoadsNoClock` (ALCLOCK-1's tests replaced it, r7 item 3). Not a finding |

## Teeth set 2 (Ruling ALCLOCK-T2): CAUGHT

- **Tool.** My own `teeth2.py`, kept in my session scratchpad as a tool, not a record:
  - Before mutating, it asserts `git status --porcelain` is empty and `git hash-object` = `git rev-parse HEAD:<file>`.
  - It waits for no other build process and for the memory floor.
  - It saves the pre-mutation bytes in memory and as a copy.
  - It applies the lane's `teeth.py` SET2 M11 text to `atx-impl/src/strategy_nav_v7.cpp`: one CRLF-aware
    replacement, asserted unique. The old condition becomes
    `if (o.aim_v6 && rate && *rate != "fixed" && *rate != nav_info_clock_rate_id)`, so the capacity curve takes any
    rate and aim-partial-v6 takes this one.
  - It runs ONE build: `scripts/research-build.ps1 -Tag p9-alclock-j -Preset equity-dev -Targets
    atx-impl-strategy-target-tests`.
  - A `finally` block restores the saved bytes, then re-asserts the clean state, and runs the gtests only after that.
  - No `git checkout`, `restore` or `stash` was used.
- **Build.** `p9-alclock-j`, configured with `atx-build.ps1 configure -Preset equity-dev` on the clean tree:
  - exit 0; 348 TUs, 13 links, 230 s, jobs 4, free 3882 MiB; 0 warning lines in `mega-p9-alclock-j-build.log`.
  - Receipt: Source `7ebd41ff`, DirtyEntries 1 (the mutated file).
  - Teeth exe `149586d2...be47`; it is never evidence of the lane's code. No other build was running.
- **Restore.** Right after the build, porcelain was 0 lines and hash-object = HEAD blob `7ebdabfd3950a28c...`.
  File SHA-256 was `11982b91...30c6` both before mutation and after restore. The same three facts held again
  after the gtest run.
- **`InfoClock*` on the teeth exe: 13 ran, 11 passed, 2 FAILED.**
  - `InfoClockRate.CapacityCurveAcceptsThisRateOnly` FAILED, as expected, on both halves:
    - per-name-v1 with `--capacity-curve` parsed (`:590`), and end to end it ran to exit 0 and published
      output (`:607`, `:608`);
    - aim-partial-v6 with this rate parsed (`:592`), with and without the capacity curve (`:594`).
  - `InfoClockRate.CommandLineRefusalsWriteNothing` FAILED too. The PM expected it to pass; it is an extra catch
    of the same mutation, not a defect. Case 8 (aim-partial-v6 + this rate) expects the parse-time usage error
    (exit 2). With the parse line loosened, the v6 runtime guard (`strategy_nav_v7.cpp:664`, "the per-name rate is
    not part of the rule") refuses at exit 1 instead.
  - So aim-partial-v6 has a second, runtime line of defence. Per-name-v1 with the capacity curve has none: the
    parse line is its only guard (pre-existing; see S-5).
- The other 11 `InfoClock*` tests passed under the mutation, as they should.

## Quality verdict: good; nothing unsafe to merge

- **Look-ahead (traced).**
  - The rate vector and the census read `ea_days_since` row t only, set on the calling thread inside
    `if (s.decision)` (`nav_session.cpp:358-366`). DECIDE runs later, in `close_book` (`nav_session.cpp:235`).
  - The field's own lag puts row t's information at the 22:00 UTC mark of t-1. A release is "usable from the
    session after the first session whose 22:00 UTC mark follows its acceptance" (`research_fields_sec.py:154`,
    computed at `:868`, `:883-898` via `Latest(ea["usable"])`). So every release in row d was accepted before mark(d-1),
    its reaction session is <= d, and `since >= 0` by construction. A negative value (which would mean a peek) is
    refused as out of domain (`nav_validate.cpp:190-196`, `nav_decide.cpp:24-28`).
  - `EventClockIsCausal` shows that no row after d0 is read. `StepChangeClosedForm` shows that row d (not d-1) sets
    decision d's theta. Members are read at row d.
- **Determinism.** The rule passes the `--book-workers > 1` gate (`nav_lockstep.cpp:213-217`) because the rate
  vector is one shared buffer written only between the phases on the calling thread. The existing SAFETY comment
  (`nav_session.cpp:334`) covers it, and books get it through `const Construction&`. The census is per book, in
  index order. The 1-vs-4 byte test with the rule on passes (lane logs; in the named filter).
- **`/W4 /WX`.** 0 warning lines in my build of HEAD's code (plus the one mutated line), and the lane's `-h` build
  also had 0. **100 columns**: every added C++ line is <= 100. One pytest line (`test_info_clock_rule.py:66`) is
  122 characters; there is no Python limit in the repo and other test files exceed 120.
- **Modular.**
  - The rule is one kernel (`nav_info_clock_rates`) declared in `strategy_nav_replay.hpp:148`, plus one
    predicate (`nav_info_clock_fast`) shared by the kernel, the census, the recipe text and the tests.
  - The rates are Y-5's engine functions (`two_speed_fast_theta` / `two_speed_slow_theta`); theta logic is not
    duplicated.
  - The rule reaches the replay through the existing per-name-rate seam (`plan_weights(..., rates, ...)`).
  - The registry row and the parser are one unit each. No allocation per decision: the vector is sized once
    (`nav_session.cpp:305`).
- **Flag-absent identity.** Shown in-exe by `FlagAbsentLoadsNoClock`, which compares a manifest listing the field
  against one that does not, after the SHA substitution list. Byte identity against the pre-lane exe is the
  canary goldens (15 / 15 Debug, lane logs) plus the gate's P9-B0 parents (TEST-1). `--help` and `--list-rules`
  output change by design.
- **Registration promises only printed figures.** Each one maps to a producer:
  - fast share and turnover share: `info_clock` block;
  - mean and max realised gross: `exposure.mean_gross_leverage` / `max_gross_leverage`, `nav_write.cpp:371-372`;
  - daily gross: daily CSV `gross_leverage`;
  - cost per traded dollar and net Sharpe at 4x: `capacity_curve.csv` columns, `strategy_nav_v7.cpp:369`, `:383`;
  - `ann_mean_gross` (C3-9).
  - `expected_effect` holds the plan's prior estimates, not statistics.
- **Seal / data.** No new path; the clock comes in through the pinned `--fields` manifest at the role's geometry.
- **Blind.** I found no 2020-2023 statistic in the report, registration or template, and I opened none.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `atx-impl/src/strategy_nav_v7.cpp:198-204` (published at `:295` recipe v7 `capacity` and `:465` `v7_extras.json` `capacity_rule`); help `:1012-1013`; message `:1344-1345` | Suggested (S-1) | Under ALCLOCK-3, P9-C's own published capacity declaration still says "fixed rate only" while the cell runs `info-clock-rate-v1`. The help text says the same, and the refusal message ("need the fixed rate (per-name-v1 reads the NAV)") no longer describes the capacity half | Add a conditional clause under the rule only, so flag-absent bytes do not move: e.g. "; P9 AL-CLOCK (Ruling ALCLOCK-3): or info-clock-rate-v1, which reads no NAV". Fix the help and message text; the test needles would follow |
| `scripts/specs/p9/rules/info-clock-rate-v1.json:51` (`realised_leverage`) | Suggested (S-2) | "so the realised gross the OD5-7 cap reads is the parent's" predates ALCLOCK-4. The ruling has the cap test read the CELL's own realised gross (mean and max, from its `summary.json`), printed beside the parent's. The pm6-6-engine match equalises only the mean, on TRAIN | Either reword this one sentence (new SHA-256; re-quote it in "For PRE"), or let prereg round 2b-ii state that ALCLOCK-4's line supersedes it. The ruling says "no code change", so either form is fine |
| `atx-impl/tests/strategy_nav_info_clock_test.cpp` (no test) | Suggested (S-3) | P9-C's NAV runs with `--calibrate-gross` (gross_match pm6-6-engine, `scripts/wave_rules.py:17`) on Y-F0's flags (`--capacity-curve --locate-in-aim --liquidity-cache --order-basis delta --warm-start-sessions 60`). No gtest runs the rule through the calibrate pass (pass 1 + tape restore + rates recomputed). The tiny world has no `ea_days_since`, so root's probe cannot run it either. I read the code and found no defect: the rates are recomputed from row t after `restore_construction` | One in-process gtest on the World fixture: rule + print + `--capacity-curve --calibrate-gross G` exits 0; the published L is the calibrated one; the blocks are present; the 1-worker and 4-worker runs are identical bytes |
| `scripts/tests/test_info_clock_rule.py:61-80` (`NAV_TABLE`) | Suggested (S-4) | The fake NAV listing is a hand literal of the row; a drift from the exe shows only at root's probe (the lane says so) | Pin the row like D1's `composition_rules_list.json`: a committed fixture, checked against `--list-rules --json` by a gtest, and read by the pytest |
| `atx-impl/src/strategy_nav_v7.cpp:1342` (pre-existing design) | Suggested (S-5, P10) | My teeth showed the v7 parse line is the only guard against per-name-v1 with `--capacity-curve`: loosened, the run publishes (exit 0). aim-partial-v6 has the runtime guard `:664`; this pair has none | Defence in depth, outside this lane: a `validate_nav_config`-level refusal of per-name-v1 on capacity books |

## What I verified myself vs from the lane's logs

| check | source | result |
|---|---|---|
| merges plain (5) | my run (`git merge-tree --write-tree`) | all five equal their commit trees |
| code unchanged after `1cbe5233` / after `adf1c6cb` | my run (`git diff`) | empty outside `.superpowers` |
| teeth set 2 (one Debug build `p9-alclock-j`) | my run | caught: 2 FAILED / 11 passed; tree restored and asserted |
| `/W4 /WX` on HEAD's code | my build | 0 warning lines |
| `scripts/tests/test_info_clock_rule.py` | my run: `PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=<s> "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider --basetemp <scratch>/bt/s<s> scripts/tests/test_info_clock_rule.py` in pool-30 | seed 0: exit 0, `5 passed in 5.62s`; seed 1: exit 0, `5 passed in 4.40s`; tree clean after |
| named filter `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:NavAnnMeanGross.*:CostLawRegistry.*:TargetRegistry.*` 24 / 24 on clean code | lane logs (`gt-h-named.log`, `gt-exits.txt:7`) | 24 passed. EXE-CUR / SPDLOG-RELINK: the run's exe SHA prefix `822de6dea00e6808` = the `mega-p9-alclock-h-receipt.json` target-tests SHA `822de6de...7626` (receipt Source `adf1c6cb`, DirtyEntries 2 = tracked logs) |
| canary 15 / 15 (Debug) | lane logs (`mega-p9-alclock-h-canary.log`, receipt Canary ExitCode 0) | 15 passed |
| teeth set 1 (7 / 7 caught) | lane logs (`gt-f-teeth.log`, pool-8 build `p9-alclock-f`) | 7 FAILED / 6 passed, as reported |
| whole target-tests, Release, whole `scripts/tests`, stage chain | deferred to the gate (TEST-1) | the last whole run is R8's 403 / 403 at `p9-alclock-e`, with the same lane code (lane logs) |

## What root must check at merge

1. **Merge target.** Merge `feat/p9-alclock2-20261007` (pool-30) at `7ebd41ff` plus this review commit, not pool-8's
   branch (TEETH-POOL). AL-CLOCK merges BEFORE COV-MV (W3-9).
2. **C3-5 probe with `--rate info-clock-rate-v1` (C3-13).**
   - Run `m2h-logs/m2h2-engine-o1.py`'s kind-rule wave naming `info-clock-rate-v1` on real exes.
   - Expected NAV argv: `--rule aim-partial-v5` exactly once (from `nav.rule`) and `--rate info-clock-rate-v1`
     exactly once (the append route). A parent `--rate` is replaced, never duplicated. A duplicate or conflicting
     `--rule` / `--rate` is a stop for the PM.
   - Note: the tiny world's fields (`shares_out`, `si_shares`, `tiny_signal`) have no `ea_days_since`. So a probe
     wave that runs past the spec stage into the NAV is refused at admission ("fields lack ea_days_since", before
     output). The token evidence is at the spec / argv stage.
   - If D3's C3-13 fix (`wave_kinds.py`) is merged first, re-run `scripts/tests/test_info_clock_rule.py` after
     both are in.
3. **W3-9 keep-both with COV-MV's `mv-aim-v1`.** Expected conflict regions:
   - `nav_rules.cpp`: this lane's `info_clock_refuses` `:57-58` and its row at the end of `trade_rates[]` `:75-82`,
     against COV-MV's target row and its refusals.
   - `nav_spec.cpp:124`: both append a key at the table's end.
   - `strategy_nav_v7.cpp`: COV-MV's `--target-rule` handler near `:1113`; this lane's ALCLOCK-3 line `:1340-1345`.
   - `nav_lockstep.cpp:69-76`, the `nav_workspace_reserve_bytes` per-name sum (COVMV-1): this lane's `clock` term
     plus COV-MV's one term. Keep both. Both flag-absent tests must pass: `InfoClockRate.WorkspaceReserveOneRatePerName`
     (rule adds exactly names x 8) and COV-MV's reserve test.
   - `atx-impl/tests/CMakeLists.txt` tail: two `target_sources` blocks.
   - Confirm with the PM / COV-MV whether `mv-aim-v1` and `info-clock-rate-v1` compose (neither row lists the
     other in `incompatible[]`; no P9 cell names both).
4. **SQL2-CLS.** None added by this lane; `classes.json` is unchanged for AL-CLOCK.
5. **The 1-vs-4 `--book-workers` byte test on root's exe.**
   - Build target-tests at the merge (EXE-CUR: my and the lane's object trees are deleted; my teeth exe was never
     evidence).
   - Run the named filter above; it includes `InfoClockRate.BookWorkersOneAndFourSameBytes` (rule + cost-v2 +
     capacity + print, 4 workers = 1 worker byte for byte).
   - Run `scripts/tests/test_info_clock_rule.py`.
6. **Before the first P9 cell.**
   - W1-I2: re-pin the parents (the NAV exe changed).
   - ALCLOCK-1: name `--info-clock-print` on the Y-F0 reference at the wave-3 P9-B0, beside `--ann-mean-gross`.
   - Y-F0's `--fields` manifest must list `ea_days_since` point in time. Otherwise both the cell and the reference's
     print are refused before output.
   - OD3P moves `info-clock-rate-v1` from `rules_pending` to `rules` (ALCLOCK-2).
   - ALCLOCK-4's prereg line goes into round 2b-ii (S-2).
7. **At the gate (TEST-1).**
   - Debug whole target-tests; the Release build, named and whole; canaries on both goldens; whole `scripts/tests`
     seeds 0 / 1.
   - The lane's flag-absent procedure (report "Flag-absent identity procedure"): X-5 and Y-F0 via argv and
     `--spec` byte-identical to P9-B0; the opt-in on Y-F0 changes only `summary.json`, by the blocks.

## Housekeeping

- Disk: I deleted my `p9-alclock-j` object tree in `C:/atx-wt/pool-30/build-equity` by literal names (`CMakeFiles`,
  `atx-core`, `atx-engine`, `atx-impl`, `atx-tsdb`, `bin`, `lib`, `tests`, `.ninja_*`, `CMakeCache.txt`,
  `build.ninja`, `compile_commands.json`, the cmake / cpack files, `spdlog.pc`, `vcpkg-manifest-install.log`).
  About 2.6 GB freed; `build-equity` is now 2.7 MB. Kept: the tracked `audits/` and
  `v8-interim3-pitch-render-run2/`, and the `mega-p9-alclock-{h,j}-*` receipts and logs. My pytest basetemps
  were deleted. C: 58 GB free.
- Permission refusals: none. Pool-8, `C:/atx`, `atx-db/`, pool-2 (read only), pool-24, `main` and leases were not
  touched. No push.
- The tree was clean before and after every step. This file is the only commit.
