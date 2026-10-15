# Re-review 1: lane AL-CLOCK, fix round 1 (ALCLOCK-S)

**Verdict: APPROVE.** 0 Required, 2 Suggested. Spec: every item of ALCLOCK-S done (S-3 REQUIRED, S-1, S-2, S-4);
S-5 is P10 by the ruling. Quality: nothing unsafe to merge; flag absent keeps every output byte by reading, and
the rule's arithmetic is untouched.

- Lane / pool: AL-CLOCK, `C:/atx-wt/pool-30`, branch `feat/p9-alclock2-20261007`. Pool-8 (FROZEN) not touched.
- HEAD reviewed: `f892eab1` (lane head; records). BASE: `e9b3a323`, the first review's APPROVE commit (code
  `1cbe5233`), per REV-BASE and FU-REVIEWS. Scope: `e9b3a323..f892eab1` only.
- Resumed review (REV-FRESH, ALCLOCK-REV-RESUME). A PARTIAL of this file was committed at `364391e0` (OWNER STOP
  10; no diff read, no finding). I am a fresh reviewer; this text replaces the PARTIAL. I ran its nine "Exact next
  step" items; step 8's "M5" now reads M6 (ALCLOCK-REV-RESUME).
- Read: ROOT-SDD `resume4-dispatch.md`, `resume6`..`resume11-addendum.md` (TEST-1 in `resume9`),
  `r7-review-dispatch.md`, `r8-review-notes.md` "AL-CLOCK", `briefs/review-template.md`, `r10-dispatch.md`
  "ALCLOCK-FIX1" and the AL-CLOCK bullet of "Scoped re-reviews", `r11-dispatch.md` "ALCLOCK-REREVIEW-1". Ledger
  by grep: ALCLOCK-1 `:625`, ALCLOCK-4 `:702`, ALCLOCK-S `:745`, its dispatch `:746`, ALCLOCK-S1a `:760`,
  ALCLOCK-S3a `:761`, hand-back `:828`, ALCLOCK-M `:829`, dispatch `:830`, ALCLOCK-REV-RESUME `:844`, dispatch
  resumed `:856`, PRE-40 `:623`, EXE-CUR `:567`, TEETH-RESTORE `:674`, TEST-1 `:688`, MERGE-SHA `:696`, HOST-4
  `:774` / `:851`, REV-FRESH `:775`, HOST-5 `:788`, TEETH-CRLF `:848`. In pool-30: the PARTIAL,
  `task-AL-CLOCK-review.md` (S-1 .. S-5), `task-AL-CLOCK-report.md` "For PRE" and "Fix round 1 (ALCLOCK-S)",
  `handoff-AL-CLOCK.md` (head line and both fix-round sections), `alclock-logs/` (receipts, build outputs, gtest,
  pytest and teeth logs, tools), `.agents/cpp/agent.md` section 10.

## The range

- `git log --first-parent e9b3a323..f892eab1`: 11 commits, three of them root merges. Each one is plain:
  `git merge-tree --write-tree <p1> <p2>` gives the commit's own tree.

  | merge | lane parent | root parent | recomputed tree = commit tree |
  |---|---|---|---|
  | `65ec16af` | `a662d1c2` | `b182dfdd` | `40c14e60...` yes |
  | `1d603d23` | `bcade871` | `858a4fca` | `ac817718...` yes |
  | `38967882` | `ee70a69f` | `5c811b56` | `19de2885...` yes |

  The three root parents are on `feat/platform-p9-20261003`, and `e9b3a323` is an ancestor of `5c811b56` (root
  M4 merged it at `858a4fca`).
- The lane's own hunks: `git diff 5c811b56 f892eab1` outside `.superpowers` (`38967882`'s root parent; the merge
  is plain). 7 files, +310 / -28. `git diff 38967882 f892eab1` outside `.superpowers` is empty: the last commit
  holds records only. Code is in three commits only: `170ac2b2` (S-3 test, S-4 fixture), `80b7ae69` (S-2
  registration, S-4 / S-2 pytest), `bcade871` (S-1). `a662d1c2`, `9d3916b8` (sprint-dir build wrapper),
  `b96fe073`, `ee70a69f` and `f892eab1` touch only `.superpowers`.

## Spec verdict (item by item)

| item | verdict | where / how checked |
|---|---|---|
| S-3 (REQUIRED, ALCLOCK-S / ALCLOCK-S3a shape) | done | `atx-impl/tests/strategy_nav_info_clock_test.cpp:765` `InfoClockRate.CalibrateGrossKeepsTheRatesAndCountsOnce`. Runs the rule (`aim-partial-v5 --cadence 1 --trade-fraction .05 --dust-multiple .1 --exit-rate .05 --rate info-clock-rate-v1`) with `--order-basis delta --liquidity-cache --capacity-curve --info-clock-print` under `--calibrate-gross` in process (the pm6-6-engine match stage). Asserts the one-process run equals the plain run at L' (every file except `summary.json`, plus the holdings tree); `summary.json` minus `calibration` equals the plain run's; `corrected` and `construction_reused` are true; L' is finite, in [1, 2] and bit-equal to `leverage_4_decimals`; `gross_requested` / `gross` / `matched` agree with values computed independently; the recipe rate is the id (main and `capacity/`); the closed form holds at L' on the calibrated holdings, from row d of the clock, on both fast and slow rows; each main block counts pass 2's member-decisions once (against a census taken from the daily CSV); `capacity/summary.json` has no block; 4 workers give the bytes of 1. The three flags left out (`--neutralize price-risk-v1`, `--locate-in-aim`, `--warm-start-sessions`) are stated in the test comment, as ALCLOCK-S3a allows. Passes on `-k` and `-m`; teeth `-l` caught (below) |
| S-1 (a) help (ALCLOCK-S1a) | done | `strategy_nav_v7.cpp:1070-1071` (`--capacity-curve`) and `book/nav_cli.cpp:95-96` (`--book-workers`). Both are help text only, and both are accurate against the predicates (`strategy_nav_v7.cpp:1438-1440`, `nav_lockstep.cpp:215-216`, both untouched) |
| S-1 (b) refusals (ALCLOCK-S1a) | done | Messages only, predicates untouched: `strategy_nav_v7.cpp:1441-1442` and `nav_lockstep.cpp:218-220`. The new v7 text is needled at `:628-629` (case 8) and in the parse matrix at `:671-673`. No Python test or golden needles any old text (my `git grep` of the three old strings at `f892eab1`: only the C1 report quotes the old lockstep text) |
| S-1 (c) capacity sentence, conditional | done | `capacity_info_clock_declaration` and `capacity_rule(bool)` at `strategy_nav_v7.cpp:215-226`. `capacity_declaration` (`:205-211`) is unchanged byte for byte and is selected whenever the rule is off. The selector is fed from: `State::info_clock` (`:92`, default false), set in `configure()` `:875` before the early return; the summary `:613`; the holdings manifest `:1049`; `v7_extras.json` `:497`; and the recipe by its own `rate` key `:1019-1021` (`nav_write.cpp:252-253` writes the id only under InfoClockV1; per-name-v1 writes its own id, which compares false). Not pinned: `git grep "capacity by replay (R4.2)"` finds only the `.cpp`, so PRE-40 does not bind. Test: `CapacityCurveAcceptsThisRateOnly` (`:700-722`) checks that under the rule all three published sentences name the rate and lack "fixed rate only", and that a fixed-rate capacity run keeps "; fixed rate only; x1 is S2 bit for bit" and has no "info-clock". `FlagAbsentLoadsNoClock` is untouched and passes on `-k` / `-m` |
| S-2 (ALCLOCK-4) | done | `scripts/specs/p9/rules/info-clock-rate-v1.json:51`, the `realised_leverage` line only. It reads "Rulings C3-4 and ALCLOCK-4" (C3-4 kept). It carries ALCLOCK-4's bound verbatim ("planned gross <= (theta_f / theta_s) x gross(aim) by contraction outside the dust band, 2.589x at cadence 1", followed by "; P9-C's mean gross matched to its parent by pm6-6-engine", where the ruling's question has ", with P9-C's ..."; not a finding). It carries ALCLOCK-S's realised clause verbatim ("realised gross leverage is read from the cell's own summary (max and mean over the cell) and printed beside the parent's"), the producer keys, and "the OD5-7 cap test applies to that realised figure". The superseded clause is gone. SHA-256 of the LF blob: `e9b3a323` `51b8b754...e33c`, `f892eab1` `7801c19880a225a2f4c6ecb8756117f03fccafa71b92f440830bfb6fdcb8a152` (my `git show | sha256sum`; the working bytes give the same raw SHA-256). Re-quoted in the report's "For PRE" (old and new). Pinned nowhere: `git grep` for both SHAs finds no code or spec at `f892eab1` or in root's tree outside `.superpowers`, and nothing in root's `p9-prereg.md` / `p9-prereg-flags.md`, so the edit is allowed (ALCLOCK-S, PRE-40). Template `95d4104a...` unchanged (no diff). The pytest asserts both clauses, ALCLOCK-4, the OD5-7 sentence and the old clause's absence (`test_info_clock_rule.py:251-259`); my teeth show it has teeth (below) |
| S-4 | done | `atx-impl/tests/fixtures/nav_rule_info_clock_rate_v1.json`, the row alone, LF blob. The lane's `mk_row_fixture.py --check`, which rebuilds the row from `nav_rules.cpp`'s own literals (`:58-59`, `:75-82`), prints True in my run. `RegistryRowAndParser` asserts `*listed == pinned` (`:883-887`, via `ATX_IMPL_TESTS_DIR`) and passes on `-k` / `-m`. The pytest's `NAV_TABLE` last row is `json.loads` of the fixture (`test_info_clock_rule.py:48`, `:80`) |
| S-5 | P10 by ALCLOCK-S | nothing in the lane; correct |
| r10 ALCLOCK-FIX1 items 1-4 | done | slices committed; `p9-alclock-k` is the round's Debug build; teeth `p9-alclock-l` was run (no other build waiting, per `builds.txt` and `teeth-s3.log`); root merged when it moved in NAV code (`38967882`); the named filter was re-run on the merged tree (`-m`). Report section and handoff head line written; object tree deleted (pool-30's `build-equity` holds only `audits/`, `v8-interim3-pitch-render-run2/` and receipts / logs) |
| ALCLOCK-M | as ruled | `-m` accepted as the merge-affected check |
| "For PRE" re-quote | done | report `:269-272` (new SHA, old SHA, and why) |

## Hunk classification (PARTIAL step 5)

| file | hunks | class |
|---|---|---|
| `atx-impl/src/book/nav_cli.cpp` | `--book-workers` help clause | text (help) |
| `atx-impl/src/book/nav_lockstep.cpp` | comment `:213-214`; refusal string `:218-220` | comment, text (refusal); predicate unchanged |
| `atx-impl/src/strategy_nav_v7.cpp` | `State::info_clock` + `configure()`; the new constant + `capacity_rule`; `declarations(..., info_clock)` and its four call sites (summary, extras, recipe, holdings); help `:1070-1071`; refusal string `:1441-1442` | selector (rule on only); text |
| `atx-impl/tests/fixtures/nav_rule_info_clock_rate_v1.json` | new | fixture |
| `atx-impl/tests/strategy_nav_info_clock_test.cpp` | includes; helpers `:249-331`; needles; capacity-sentence checks; the S-3 test; fixture check | test |
| `scripts/specs/p9/rules/info-clock-rate-v1.json` | `realised_leverage` | registration |
| `scripts/tests/test_info_clock_rule.py` | docstring; `NAV_ROW_FIXTURE`; `NAV_TABLE` row; S-2 asserts | test |
| `.superpowers/.../alclock-logs/tools/` | `build.sh`, new `pool-builds.ps1` (HOST-4 / HOST-5 wait; research-build's memory gate kept, not lowered); `teeth_s3.py`; `mk_row_fixture.py` | lane tools (records) |

- No default changed: the one new member is `bool info_clock{}`, false.
- No key is output unconditionally: `declarations()` still writes `capacity` only under `s.options.capacity`, and
  `extras["capacity_rule"]` already existed. Only the value is selected.
- The rate's arithmetic is untouched: `nav_decide.cpp`, `nav_session.cpp`, `nav_rules.cpp` and the kernel are not
  in the lane's diff.
- No new `atx.<name>/v<n>` literal, flag, JSON key or rule id. The CMake tail is unchanged.

## Quality verdict: good; nothing unsafe to merge

- **Flag absent.** With the rule off, `capacity_rule(false)` returns the old constant. `extend_recipe`'s
  `info_clock` is false without the id (no `rate` key, or `per-name-v1`), so every recipe / summary / holdings
  manifest / `v7_extras.json` byte of a run without the rule is unchanged by reading. The `cap-fixed` assertion
  shows the old text on the fixed path. What changes by design: the `--help` text (two clauses) and two refusal
  messages, which belong to refused runs that write nothing (ALCLOCK-S: help / refusal text). Byte identity
  against the pre-merge exes is the gate's (TEST-1; P9-B0 parents).
- **Tests pin the contract, not the implementation.** The S-3 test checks PM6-6's contract (one process =
  two-process at L') and the rule's definition (closed form from row d of the clock). It checks
  `gross_requested` / `gross` / `matched` against values computed from published files, and the census against
  an independent daily-CSV census. Teeth show the rates' survival through the restored pass is what the bytes and
  the closed form catch.
- **Determinism.** The 4-vs-1 `--book-workers` byte test is repeated under the calibrate pass (`:844-853`).
- **`/W4 /WX` and 100 columns.**
  - `-k` / `-l` / `-m` all exit 0 with 0 warning lines (`builds.txt`; `grep -c warning` = 0 in all three build
    logs). `-m`'s ninja output shows `strategy_nav_v7.cpp`, `nav_cli.cpp`, `nav_lockstep.cpp` and
    `strategy_nav_info_clock_test.cpp` compiled on the merged tree. `/WX` comes from `atx_warnings`
    (`CMakeLists.txt:274`).
  - The single-TU checks of the four TUs are clean (`check-fix1-s1.log`, `check-fix1-s3-test.log`; the only
    warning lines are the third-party spdlog `/MP` notes).
  - My scan of every added C++ line: none over 100 columns, no trailing blank, no tab.
- **Modular / reuse.**
  - The selector follows the file's precedent (`capacity_spo_v3_rule(adv_hold)`).
  - Rates come from the engine functions (`st::nav_info_clock_fast_theta`, `bk::two_speed_slow_theta`), and the
    calibrate arithmetic from `st::numpy_mean` / `st::leverage_4_decimals`.
  - The test helpers `all_rows_gross` / `four_decimals` repeat the calibrate test's copies (S-B).
- **Look-ahead / seal / blind.** The fix round adds no data path. The closed form reads row d of the fixture clock
  for decision d. Synthetic fixtures only. I opened no real data and read no 2020-2023 statistic.

## Teeth

- **The lane's S-3 teeth `p9-alclock-l` (verified from its log and receipt; EXE-CUR).**
  - Tool `alclock-logs/tools/teeth_s3.py`, committed at `b96fe073` before its run. Before mutating, it asserts
    porcelain empty and hash-object = HEAD blob.
  - It keeps the bytes in memory and as a copy, and makes one CRLF-aware replacement, asserted unique, in
    `nav_session.cpp`: `if (clock_rule)` -> `if (clock_rule && !(tape && tape->replay))`.
  - Its `finally` restores the bytes and asserts raw SHA-256 = before, hash-object = HEAD blob and porcelain empty.
    This is the TEETH-RESTORE / TEETH-CRLF form; no checkout / restore / stash.
  - `teeth-s3.log`: mutated, built, then "restored: sha256 c86532ab4c83f84f (before c86532ab4c83f84f),
    hash-object = HEAD blob True (0aa59660aec0bc17), porcelain empty True -> OK".
  - Receipt `mega-p9-alclock-l-receipt.json`: Source `b96fe073`, DirtyEntries 1 (the mutated file; porcelain
    at mutation `M atx-impl/src/book/nav_session.cpp` only), 1 TU, exit 0. Exe SHA-256 `202aad461e912dbb...e0` =
    the `gt-exits.txt` `l-teeth` line's exe prefix `202aad461e912dbb`.
  - `gt-l-teeth.log`: 14 ran, 13 passed, 1 FAILED, and the one is `CalibrateGrossKeepsTheRatesAndCountsOnce`.
    Failure sites by line: `:81` x14 (published files differ from the plain run at L'), `:72` x3 (holdings),
    `:326` x194 (closed form), `:805` x1 (summary minus calibration), `:815` x1 (published gross).
  - `b96fe073`'s code equals `9d3916b8`'s (`-k`'s source; empty diff outside `.superpowers`).
    `nav_session.cpp`'s blob is `0aa59660...` at both `b96fe073` and `f892eab1`, so the mutation site is
    unchanged at the fix head.
- **My own Python teeth for S-2 (TEETH-RESTORE / TEETH-CRLF).**
  - Tool `teeth_s2.py`, in my session scratchpad (a tool, not a record). Before mutating it asserts porcelain
    empty and hash-object = HEAD blob, and it keeps the bytes in memory.
  - It replaces the registration's `realised_leverage` line by `e9b3a323`'s line (the S-2 fix reverted) and runs
    the pytest once, seed 0, own `--basetemp`.
  - Result: exit 1, `FAILED ...::test_registration_and_template`, `1 failed, 4 passed`.
  - Restore: raw SHA-256 `7801c19880a225a2` = before, hash-object = HEAD blob `f858e08ca3c967b0` True, porcelain
    empty True: OK. Re-checked afterwards: porcelain 0 lines, hash-object = `git rev-parse HEAD:<path>`.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `atx-impl/tests/strategy_nav_info_clock_test.cpp:692-713` (`CapacityCurveAcceptsThisRateOnly`); claim at `atx-impl/src/strategy_nav_v7.cpp:223` | Suggested (S-A) | S-1 (c)'s new sentence, published under the rule, keeps the claim "x1 is S2 bit for bit". The exe computes that fact at runtime (`v7_extras.json` `capacity_x1_equals_primary_bit_for_bit`, `strategy_nav_v7.cpp:499-500`), but no test asserts it with the rule on; only fixed-rate tests do (`strategy_live_test.cpp:2265`, `:2423`). By reading it holds (one rate vector per decision for every book; x1's law equals S2's), but the published claim has no test | Add one `EXPECT_EQ(Json::parse(bytes_of(w.out("cap") / "v7_extras.json")).at("capacity_x1_equals_primary_bit_for_bit"), true)` in this test (optionally the same on S-3's `calibrated` run). Test only; flag-absent bytes unaffected |
| `atx-impl/tests/strategy_nav_info_clock_test.cpp:249-268` (`all_rows_gross`, `four_decimals`) | Suggested (S-B, P10) | These copy `csv_all_rows_gross` / `four_decimals` of `strategy_nav_calibrate_test.cpp:77-106`. A third and a fourth copy of the primary-gross reader are in `strategy_mv_aim_rule_test.cpp:892-913` and `strategy_nav_replay_test.cpp:3430-3447`. The copies are anon-namespace and not wrong, but four copies of the PM6-6 test arithmetic can drift | P10, cross-lane: a shared test-support header for published-directory readers (primary daily gross mean, the 4-decimal leverage text), used by the four files |

No Required finding.

## What I verified myself vs from the lane's logs

| check | source | result |
|---|---|---|
| three root merges plain | my run (`git merge-tree --write-tree`) | all equal their commit trees |
| lane-only diff; records-only last commit; code commits | my runs (`git diff 5c811b56 f892eab1`, `git diff 38967882 f892eab1`, `git show --stat` per commit) | 7 files; empty; three code commits |
| registration SHAs (old / new), template unchanged, nothing pins either SHA | my runs (`git show`, `sha256sum`, `git grep`) | `51b8b754...` -> `7801c198...`; `95d4104a...` unchanged; no pin |
| old texts not needled / pinned anywhere | my `git grep` at `f892eab1` | none outside the lane's own `.cpp` / test |
| S-4 fixture = the source's row | my run (`mk_row_fixture.py --check`, no write) | True, exit 0 |
| `scripts/tests/test_info_clock_rule.py` | my run in pool-30 at `364391e0` (code = `f892eab1`): `PYTHONDONTWRITEBYTECODE=1 PYTHONHASHSEED=<s> "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider --basetemp <scratch>/bt/s<s> scripts/tests/test_info_clock_rule.py` | seed 0: exit 0, `5 passed in 4.30s`; seed 1: exit 0, `5 passed in 3.35s`; tree clean before and after |
| S-2 Python teeth | my run | caught (1 failed / 4 passed); restore asserted OK |
| `InfoClock*` 14 / 14 (InfoClockRate 10, InfoClockPrint 4) on `p9-alclock-k` | lane log `gt-k-clock.log`; EXE-CUR: run exe prefix `44a2dd76e8f3267f` = receipt `44a2dd76...b039` (Source `9d3916b8`, ConfiguredProvenance `9d3916b8`; DirtyEntries 2 = records; `pyt-exits.txt` shows code-dirty `[]` at that head) | 14 passed, ENV-1 hits 0 |
| named `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:NavAnnMeanGross.*:CostLawRegistry.*:TargetRegistry.*` on `-k` and on `-m` | lane logs `gt-k-named.log`, `gt-m-named.log`; EXE-CUR: `-m` run exe prefix `78311aa94f0ab61d` = receipt `78311aa9...dd9e` (Source and ConfiguredProvenance `38967882`); `git diff 38967882 f892eab1` outside `.superpowers` empty, so `-m` is current for the fix head | 26 / 26 each, the calibrate test included; ENV-1 hits 0 |
| teeth `p9-alclock-l` | lane logs + receipt (above) | caught: only the new test red; restore verified |
| receipts in `alclock-logs/` = those in `build-equity/` | my run (`sha256sum`) | identical for `-k`, `-l`, `-m` |
| whole target-tests, Release, canaries, whole `scripts/tests`, stage chain, P9-B0 flag-absent identity | - | deferred to the gate (TEST-1) |

**No build by me.** Pool-30 holds no object tree (`build-equity` holds only `audits/`,
`v8-interim3-pitch-render-run2/` and receipts / logs). Every item is proved from the lane's logs and receipts
(EXE-CUR) plus my own git, pytest and Python-teeth runs, so tag `p9-alclock-n` is unused (HOST-4).

## What root must check at merge (M6)

1. **Second AL-CLOCK merge, at the fix head.**
   - Merge `feat/p9-alclock2-20261007` (pool-30) at this review's commit (MERGE-SHA). That commit is the docs
     commit above `f892eab1` / `364391e0`, and its code is `f892eab1`'s.
   - The first AL-CLOCK merge is `858a4fca` (`e9b3a323`), so this one brings only fix round 1: the 7 files
     listed above plus records. COV-MV is already in the branch (`38967882`); this is not a keep-both step.
   - My predictive `git merge-tree --write-tree` of `f892eab1` into root's head at this review (`138e1d2c`)
     is clean. Re-check against root's then-current head after M5's other merges.
2. **The named filter on root's Debug `atx-impl-strategy-target-tests`.**
   - A fresh build is needed: the lane's object trees are deleted (EXE-CUR).
   - Run `InfoClock*` (14 / 14 at the fix head, including
     `InfoClockRate.CalibrateGrossKeepsTheRatesAndCountsOnce`, the calibrate gtest) and the named filter above
     (26 / 26 at the fix head).
   - A count change after root's other merges belongs to the lane whose test it is.
3. **`scripts/tests/test_info_clock_rule.py` at seeds 0 and 1** (5 / 5 each at the fix head).
4. **The registration sentence (ALCLOCK-4).**
   - `scripts/specs/p9/rules/info-clock-rate-v1.json:51` carries both clauses. SHA-256 (LF) is now
     `7801c19880a225a2f4c6ecb8756117f03fccafa71b92f440830bfb6fdcb8a152` (was `51b8b754...`, the one merged at
     `858a4fca`). It is pinned nowhere yet; W1-I2's re-pin at the gate reads the new SHA.
   - Template `95d4104a...` is unchanged.
   - The prereg's P9-C block (round 2b-ii) carries the same two clauses (ALCLOCK-4; PRE's).
5. **Flag absent, byte for byte.**
   - For a run without `--rate info-clock-rate-v1`, only the `--help` text and two refusal messages change, by
     design. `capacity_declaration` is unchanged and selected whenever the rule is off.
   - At the gate (TEST-1): the P9-B0 parents and the lane's "Flag-absent identity procedure" (report). The
     `--capacity-curve` parents at the fixed rate must keep "; fixed rate only; x1 is S2 bit for bit" and every
     byte.
6. **The fixture against the merged registry.** `RegistryRowAndParser` picks the row by id, so COV-MV's row
   (`targets[]`) and any later `trade_rates[]` append leave the row fixture valid. The lane's
   `mk_row_fixture.py` regex expects the row last; it is a lane tool, not a test.
7. **Ledger.** Until `Task AL-CLOCK: APPROVE at <this review commit>` is written, `e9b3a323` stays the SHA of
   record (ALCLOCK-REV-RESUME).

## Housekeeping

- Permission refusals: none.
- Not touched: pool-8, pool-24, `C:/atx`, `atx-db/`, `main` / `origin/main`, any lease. Pool-2 was read only. No
  push. No data opened; `holdout_gate.py` not run.
- Build: none (`p9-alclock-n` unused), so there is no object tree to delete. Disk freed: my scratch only (pytest
  basetemps, diff copies, the teeth tool; under 1 MB).
- Tree clean before and after every step. This file is the only commit.
