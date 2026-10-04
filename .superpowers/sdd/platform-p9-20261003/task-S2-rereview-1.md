# Lane S2 re-review 1 (fix round 1) -- COMPLETE

## Verdict

**APPROVE** (0 Required, 1 Suggested).

- Every S2-FIX1 item in scope (1, 2, 4, 6, 7, 8, with 8 as amended by S2-5) is implemented, and every
  new or extended test has teeth against the defect it covers (Part 2, A).
- The re-pin `ic_result_sources` `bd13574c -> dd3c6011` reproduces from the git blobs. `dsl_vm_sources`
  is unchanged at `5bcd55c7` (Part 2, B).
- The cited receipts and logs exist and say what the report says. The one evidence gap is Suggested 1.
- Flag-absent identity holds at the source level and on the canary.
- The round introduces no look-ahead on the label path.
- Nothing dated on or after 2024-01-01 was opened.

## Reviewed SHA

`2ed2fc18` (code `110fd735`, tests `80d81b26`, report `2ed2fc18`). Fix diff `98c85cc9..2ed2fc18`, branch
`feat/p9-s2-20261003`, pool-18. `80d81b26..2ed2fc18` touches only the report and the canary log.

Two reviewers wrote this file:
- **Part 1** is the first reviewer's PARTIAL review (commit `f6c0e964`), kept verbatim.
- **Part 2** finishes the scope and was written by a fresh reviewer.

Part 2 did not redo Part 1's items (1), (2) and (4). It re-read their code only where Part 2's own
checks needed it.

## Findings

Severity: Required = must fix before merge; Suggested = root may defer.

**Required:** none.

**Suggested:**

1. `task-S2-report.md:255-265` ("Tests at `80d81b26`" and "Flag-absent identity") | Suggested
   (evidence) | No gtest output from the fix round is kept, so the counts 204 / 782 / 44 are claims
   without a log.
   - The binaries are receipt-backed: `p9-s2-fix1-b` built them at `80d81b26` with DirtyEntries 0 (B).
   - `canary-head-80d81b26-debug-check.log` does not name the exes it ran. Only its timestamp ties it to
     `80d81b26`: 22:24, after fix1-b at 22:16.
   - So no one has independently run the two new tests (`IcLabels.TerminalImputedPairsPassBothReturnGuards`,
     `IcLabelTerminal.AbsurdTerminalReturnIsGuardedOnTheLabelRole`) or the four extended ones.
   - Fix: at merge, root runs
     `IcLabels.*:IcLabelTerminal.*:StrategyIcRunner.AuditExactEvalModeIsRecordedAndCachedUnderItsOwnIdentity:StrategyIcRunner.*SourcesPinnedToSemanticsVersion`
     in `atx-impl-strategy-ic-tests`, Debug and Release.
   - For later rounds: a lane commits the gtest summary tail and the exe SHA-256 beside its canary log.

Observations, not findings:
- Part 1 (2.4): a delisting stage with chained terminations fails closed.
- Under `imputed-v1`, `guard_excluded_pairs` pools two kinds of exclusion: the scored guard's and the
  label guard's terminal step. No counter isolates "delisting returns dropped by the guard". With the
  builder's Shumway set the terminal step can never fire (Part 1, 1.6), so the pooled count loses nothing
  today. If P9-L ever admits another return rule, it should add that counter.

## What root must check at merge

1. Run the filter in Suggested 1 at the merged SHA, Debug and Release.
2. The original review's root items still stand: X-5 u / w flag-absent identity, the cold-u wall,
   `atx-engine-factory-tests` (the unity batch holding `ic_screen_test.cpp`), the `hygiene` preset,
   Release, and the S2-4 / W1-I1 path length.
3. Do not carry pre-fix `imputed-v1` IC-result entries into P9-L.
   - The re-pin has no semantics bump (S2-1), and the source digest is in no cache key
     (`ic_identity()`, `strategy_ic_result_cache.cpp:57-62`).
   - So an `imputed-v1` entry written by a pre-fix S2 exe would be served by the post-fix exe.
   - None can exist today: S2 is unmerged, and lanes and reviewers ran it only on synthetic fixtures in
     temp dirs.
4. S2-FIX1 (3) is not a lane item: the 30-day cause-classification clock stays with the P9-L prereg.

## Checked

- [x] `.agents/cpp/agent.md` §10 applied to `98c85cc9..80d81b26`. Findings:
  - no UB, lifetime, bounds or determinism defect;
  - the label guard's span is cleared before its vector is freed (`strategy_ic_runner.cpp:399-400`);
  - every `label_bad` index is below `cells`, because `label_bad.size()==bad.size()==cells`
    (`ic_screen.cpp:161,331`).
- [x] The diff stays inside S2's files: `ic_research.hpp`, `ic_screen.cpp`, `ic_screen_test.cpp`, and the
  IC runner's admission, result-cache and runner sources and their test.
- [x] Evidence matches the claims: re-pins recomputed, receipts and logs read, pytest re-run. The gtest
  counts are not re-run (Suggested 1).

---

# Part 1 (first reviewer, PARTIAL, `f6c0e964`; kept verbatim)

**PARTIAL: no verdict.** The owner stopped this review before it finished. No Required finding so far.

- Scope: fix diff `98c85cc9..2ed2fc18`, branch `feat/p9-s2-20261003`, pool-18. Code is `110fd735`,
  tests `80d81b26`, report `2ed2fc18`.
- Inputs read: `task-S2-review.md` (Suggested 1-8), the report's "Fix round 1" section, and Ruling
  S2-FIX1 / S2-5 (pool-2 `progress.md`).
- Method: source review only. No build, no test run, and no data opened.

## Items checked

### (1) Second return guard on the label role -- PASS

1. **Same thresholds.** The label guard is `engine::data::research_return_guard(loaded)` on the label
   role (`atx-impl/src/strategy_ic_runner.cpp:389`). That is the same function, with the same thresholds,
   as the scored guard: |log close ratio| > 1.5, or adjusted log ratio > |raw log ratio| + .10
   (`atx-engine/src/data/role_panel.cpp:34-54`).
2. **What the label guard adds.** The label role is now bound bitwise to the scored role on every
   scored-present cell, for both close and raw_close (`atx-impl/src/strategy_ic_admission.cpp:440-442`).
   It may add presence only at `last[i]+1` (`:432-437`). So the label prefix differs from the scored one
   only by the single step L -> T. For an imputed pair (exit = T):
   - `label_bad[T]-label_bad[entry]` is the scored count over (entry, L] plus bad(L -> T);
   - the scored prefix carries flat over L -> T, because T is absent from the scored role.

   The OR at `atx-engine/src/factory/ic_screen.cpp:420-421` therefore adds exactly the terminal step.
3. **Scope.** The label guard is read only when `imputed`. That flag is set only after a missing exit
   whose terminal session the label role prices (`ic_screen.cpp:411-417`). Non-imputed pairs use the
   unchanged `bad[exit]!=bad[entry]` with `exit = end`.
4. **Flag-absent path unchanged.**
   - `terminal_inputs` returns `TerminalInputs{}` for MissingExitV1 before the new size and monotonicity
     checks (`ic_screen.cpp:324`). No new refusal can reach a flag-absent run.
   - `label_prices` is empty, so `imputed` stays false and the `||` short-circuits.
   - `prepare_ic_screen` passes `ResearchIcTerminal{}`.
   - The recipe `labels` text and the +4 B/cell admission term apply only under `!cfg.label_terminal.empty()`.
   - Source-level conclusion: no change on the default path. The `ic_result_sources` move is a source
     digest only.
5. **Cannot drop a pair the flag-absent path keeps.** The flag-absent path only keeps pairs with a
   present exit. Those are never `imputed`, so the label guard is never consulted for them.
6. **Builder Shumway set passes.**
   - The builder writes `close[T]=close[L](1+r)` and `raw[T]=raw[L](1+r)`
     (`atx-engine/tools/prepare_recent_research.py:157-163, 892-893`). The two log ratios are equal up
     to rounding, so the .10 drift test cannot fire.
   - |log(1+r)| is 0, .357 and .799 for r in {0, -.30, -.55}, all below 1.5.
   - The reader forces present cells to have finite positive close and raw_close
     (`atx-engine/src/data/strategy_data.cpp:151-159`), so the L -> T step is always evaluated, never
     skipped as non-finite.
7. **Shape refusals.** A missing or wrong-shape label guard is refused, and so is a decreasing one
   (`ic_screen.cpp:332-341`). `bad` empty implies `label_bad` empty, so a guard-less run stays
   guard-less in both panels.

### (2) Added presence only at `last[i]+1` -- PASS

1. **Rule matches the builder.** The builder applies a return only when the line is present at L and at
   no role session after L. It then sets `present[L+1]=1`, and skips the cases "last_role_session",
   "not_present_at_last_session" and "present_after_last_session" (`prepare_recent_research.py:886-891`).
2. **Presence is the same mask on both sides.** C++ presence is exactly `present.u8`
   (`strategy_data.cpp:146,167`). The scored role is the unpatched build (E-10), so builder L equals
   C++ `last[i]`, and builder T equals `last[i]+1`.
3. **Termination shapes.**
   - **Terminates on the last role session:** the builder skips it (`last_role_session`). No presence is
     added and nothing is refused.
   - **Gap before termination:** the gap does not matter.
     - Present at L: then T = L+1 = last+1, which is accepted.
     - Absent at the vendor L: the builder skips it (`not_present_at_last_session`).
     - An interior-gap missing exit now finds no label-only session, so the pair gets no label. This is
       the same result as MissingExitV1, and it closes review finding 2.
   - **Re-listing (same column present again later):** the earlier event is skipped by the builder
     (`present_after_last_session`). Only the final termination is applied, at the final last+1, so it
     is accepted.
4. **Observation, not a finding.** If the delisting stage held two non-continued rows for one column with
   last sessions L and L+1, the builder's loop would read its own patched `present[L+1]`. It would then
   add L+2 as well, chaining an imputed close onto an imputed close. The C++ check refuses this loudly at
   (L+2, i), which is fail-closed and correct. I did not check stage uniqueness, because atx-db is out of
   scope.

### (4) Empty TRAIN pin never written at `7ccf81e8` -- PASS (verified)

1. At `7ccf81e8`, `atx-impl/src/strategy_ic_runner.cpp`:
   - `:806` runs `roles.erase(roles.begin())` under `validation_only`;
   - this happens before the pin loop at `:872-874`;
   - `admit_label_role` for TRAIN is also skipped under `validation_only` (`:784`).

   So the pin loop never sees TRAIN, and `"train": ""` was never written. The lane's claim holds.
2. The new `Internal` refusal at head (`strategy_ic_runner.cpp:880-883`) cannot fire on an existing valid
   run. Every role still in `roles` at that point is scored. `admit_label_role` (`:784` / `:791`) has
   already refused any scored role whose pin path or SHA is empty
   (`strategy_ic_admission.cpp:389-391`).

## Items not yet checked

1. **(6) Engine test teeth.** Not checked: whether
   `IcLabels.TerminalImputedPairsPassBothReturnGuards` (A: shared scored bad return at 6; B: label-only
   terminal bad step; and its refusals) would fail on the pre-fix code.
2. **(6) Impl test teeth.** Not checked: `IcLabelTerminal.AbsurdTerminalReturnIsGuardedOnTheLabelRole`
   and its expected {5, 21, 33} guard deltas.
3. **(7) Stale-sidecar tests.** Not checked: whether the audit-root sidecar plant and the
   `missing_exit_v1` IC-record plant hit the mismatch path, rather than a miss or a different refusal.
4. **(4) Test bytes.** Not checked: that `FrozenTrainResumeBindsTheLabelRolePins` asserts the exact
   recipe bytes and SHA.
5. **`LabelRoleRefusals` case.** Not checked: that the new doubled-label-role case is refused at
   "(date 421, instrument 0)".
6. **Re-pin `bd13574c -> dd3c6011`.** Not yet recomputed from git blobs at `80d81b26` / `2ed2fc18`. Also
   not confirmed: that `dsl_vm_sources` is unchanged at `5bcd55c7`.
7. **Receipts and logs.** Not inspected:
   - the `p9-s2-fix1-b` receipt (`build-equity/mega-p9-s2-fix1-b-*`: source `80d81b26`, DirtyEntries 0);
   - the test logs;
   - `s2-evidence/canary-head-80d81b26-debug-check.log`.
8. **Item (8) and Ruling S2-5 line-length check.** Not done.

## Findings so far

None Required. None Suggested beyond the observation in (2).

---

# Part 2 (finishing reviewer): Part 1's open items 1-8, then checks (d), (e) and (f)

Method:
- Source read via `git show 2ed2fc18:<path>`, with the fix diff written to a scratch file.
- Two read-only scratch scripts (git blobs only): the digest recompute and the long-line count.
- One pytest re-run.
- The receipts and logs under `build-equity/` and `s2-evidence/`.
- No build. Every decision was reachable from source plus receipts (W2-BUILD not needed). No data
  opened, and nothing edited in the lane tree.

## Evidence

1. **pytest** in pool-18. Command (explicit path, `PYTHONDONTWRITEBYTECODE=1`, own basetemp):
   `python -m pytest -q -p no:cacheprovider --basetemp=<scratch> atx-impl/tools/test_fit_composition_rule_plugins.py`
   - Exit code 0. Output tail: `........ [100%]` / `8 passed in 0.75s`.
   - `git status --short` was empty afterwards.
2. **Source pins, recomputed from git blobs.**
   - Method: a scratch script parses each `*_sources` list from its owning .cpp at that commit, then
     applies the tripwire recipe from `strategy_ic_runner_test.cpp:2223-2248`: for each path in list
     order, `path\n<len>\n<text>`, with only CRLF -> LF, then SHA-256.
   - Result:
     ```
     98c85cc9 dsl_vm_sources     35/35 pin 5bcd55c788144513 recomputed 5bcd55c788144513 EQUAL
     98c85cc9 ic_result_sources  11/11 pin bd13574c08a61e4c recomputed bd13574c08a61e4c EQUAL
     110fd735 dsl_vm_sources     35/35 pin 5bcd55c788144513 recomputed 5bcd55c788144513 EQUAL
     110fd735 ic_result_sources  11/11 pin dd3c601163029c1d recomputed dd3c601163029c1d EQUAL
     80d81b26 dsl_vm_sources     35/35 pin 5bcd55c788144513 recomputed 5bcd55c788144513 EQUAL
     80d81b26 ic_result_sources  11/11 pin dd3c601163029c1d recomputed dd3c601163029c1d EQUAL
     2ed2fc18 dsl_vm_sources     35/35 pin 5bcd55c788144513 recomputed 5bcd55c788144513 EQUAL
     2ed2fc18 ic_result_sources  11/11 pin dd3c601163029c1d recomputed dd3c601163029c1d EQUAL
     ```
   - The full pin at `strategy_ic_result_cache.cpp:51` is `dd3c6011...2913a94c2`, equal to the report.
   - The old pin reproduces from the pre-fix blobs, so the new pin is the committed sources' digest, not
     a value fitted to a test.
   - The fix adds no `#include`, so the closure check is unaffected.
   - The changed engine files (`ic_research.hpp`, `ic_screen.cpp`) are both in the IC list and in neither
     `dsl_vm_sources` nor the marginal `pair_sources` (2 combine files). So no other pin moves.
3. **Receipts and logs** (`build-equity/mega-p9-s2-fix1-*`). The logs are UTF-16, read via `iconv`.
   - **`fix1-a`:** Source `98c85cc9` plus 7 dirty entries, ExitCode 0, 365 TUs, 457.8 s.
     - The 7 dirty entries are exactly the 7 files the fix commits change.
     - The log shows each changed TU compiled: `ic_screen.cpp`, `strategy_ic_admission.cpp`,
       `strategy_ic_runner.cpp`, `strategy_ic_result_cache.cpp`, `strategy_ic_runner_test.cpp`, and
       `ic_screen_test.cpp` into `atx-impl-strategy-ic-tests`.
     - 0 `error` / `FAILED` lines.
     - Warnings: 7 x clang-cl `/MP` unused argument and 2 x databento `getenv`. None is first-party.
   - **`fix1-b`:** Source `80d81b26`, DirtyEntries 0, ExitCode 0, 7 TUs, 6 links, 27.2 s.
     - The 7 TUs are all spdlog: no lane TU recompiled.
     - Ninja's mtime check therefore shows the 7 files were not touched after fix1-a.
     - So the five exes (SHA-256 values in the receipt) are the `80d81b26` sources, as the report says
       ("the fix-b receipt pins the exact source").
   - **Canary log:** `s2-evidence/canary-head-80d81b26-debug-check.log` says `15 passed in 12.11s`.
     Its planted-member block is synthetic tiny_world.
   - **Test logs:** none kept (Suggested 1).
     - 204 = 202 + the two new tests. This is consistent, because `ic_screen_test.cpp` compiles into
       the IC test binary.
     - 782 is unchanged: no alpha test changed.

## A. Test teeth (open items 1-5): would each test fail with its fix reverted?

**(6) Engine: `IcLabels.TerminalImputedPairsPassBothReturnGuards`** (`ic_screen_test.cpp:1022-1112`).
Fixture: L = 7, T = 8, h in {1..4}, execution delay 1. The straddling entries are [8-h, 7], and all of
their decision rows are active.
- **(B), item 1's tooth.** The scored guard `clean` is non-empty and all zero; the label guard is
  `guard_from(8)`.
  - At head: every straddling pair has `label_bad[8]-label_bad[entry]=1`, so all h pairs are
    guard-excluded and none is imputed.
  - With the OR at `ic_screen.cpp:421` reverted, the scored test is 0 for all h pairs, so they impute.
  - The assertions then fail in every horizon: `terminal_imputed_pairs` = h, not 0; the guard delta is
    0, not h; `finite_label_pairs` is +h; and the rank series moves.
- **(A), the item-6 test** (the scored guard on imputed exits, previously unexercised). The bad step
  5 -> 6 is in both guards. That is what the real label guard is, given the bitwise binding on
  scored-present cells.
  - By hand: kept = entries >= 6 in [8-h, 7] = {1, 2, 2, 2}; guarded = {0, 0, 1, 2}. The expected values
    come from the entry range and `terminal_close / close[entry] - 1`, not from the code under test.
  - (A) passes on the pre-fix code too, as it should: it pins behaviour that already existed.
  - It fails under the mutation that skips the guard for imputed exits: at h = 3 and 4, 3 and 4 pairs
    would impute instead of 2.
  - It cannot tell the scored guard from the label guard over (entry, L]. Production does not need
    that: the binding makes the two guards equal there.
- **Refusals.**
  - `with({})` beside a non-empty scored guard hits the size check at `ic_screen.cpp:331`.
  - `decreasing` hits the monotone check at `:336-337`.
  - Both checks are new, so both `EXPECT_FALSE` fail on revert.

**(6) Impl: `IcLabelTerminal.AbsurdTerminalReturnIsGuardedOnTheLabelRole`**
(`strategy_ic_runner_test.cpp:4628-4654`).
- **The plant.** `close` and `raw_close` are patched to 0.05 x (419, 0) at (420, 0) in the label role
  only, and the manifest is re-pinned.
  - (420, 0) is label-only: the scored role is absent from 420 (`absent_from(kTerminalSession)`). So
    `bind_label_role` does not bind it.
  - The value is finite and positive, so the reader admits it.
- **What the guard does.** On the label role, step 419 -> 420 gives |log .05| = 3.00 > 1.5, so it is
  bad.
  - Every straddling pair is guard-excluded. The counts are 5, 21 and 33: the score window is
    [383, 480), and at h = 63 all 33 mature rows straddle.
  - In `ic_missing_exit_v1` the same pairs stop at the missing-exit `continue` before the guard. So the
    guard delta is exactly the straddling count, `finite_label_pairs` is equal, and the `rank` /
    `pearson` blocks are equal.
- **Teeth.** Reverted, the run imputes 5 / 21 / 33 (`terminal_imputed_pairs` != 0), and the IC moves
  (the existing `ImputedRun...` test shows imputation changes the daily IC).
- **No false positive.** The unchanged `ImputedRunPrintsBothIcsCountsImputedPairsAndRecordsThePin` still
  expects 5 / 21 / 33 imputed through the new label guard. It uses the fixture's tiny terminal step, so
  it proves the guard does not fire on a normal termination.

**(2) `IcLabelTerminal.LabelRoleRefusals`, doubled case** (`:4588-4598`).
- **The plant.** `role(..., 17683, ..., 1, absent_from(422))` keeps instrument 0 label-present at 420 and
  421. Its close and raw_close are the same formula as the scored role, so they are bit-equal on every
  scored-present cell.
- **No earlier refusal.** Admission compares the score window, sessions and ids, and all equal the
  fixture's label role.
- **Order of the refusal.** The scan in `bind_label_role` is date-major. (420, 0) is `last+1` and passes.
  (421, 0) is refused with exactly "label role adds presence that is not a termination (date 421,
  instrument 0)", before `IC eval-start`.
- **Teeth.** The pre-fix `if (!scored.in_universe(d,i)) continue;` accepts the role. Then `twice.ok` is
  true and the test fails.

**(7) Audit-root stale sidecar** (`:3061-3074`).
- **The plant.** The ResearchFast sidecar of `volume_level` (train) goes into the audit root, at the
  audit key's own path.
- **Why it hits the mismatch.** `cache_lookup` finds the sidecar, so this is not a miss
  (`strategy_ic_signal_cache.cpp:354-357`). `v2_payload_sha` (`:314-320`) then refuses it before any
  payload read, on three differences:
  - `eval_mode` (`same_geometry`, `:308`);
  - `vm_identity`;
  - `signal_key_sha256`.

  The result is "candidate cache entry mismatch: volume_level", and train / `volume_level` is the first
  lookup.
- **Teeth.** With all three identity checks dropped, the foreign sidecar would be either served (stale
  run OK) or refused on payload SHA (different text). Both fail the test.
- **Restore.** After the restore, the run serves the audit daily IC.

**(7) `missing_exit_v1` scope plant** (`:4757-4768`).
- **The plant.** The flag-absent run's record for the same signal: same candidate, same DSL, same
  signal payload SHA (one shared signal-cache dir).
- **Why it hits the mismatch.** The record is intact, so it passes the malformed and integrity checks
  (`strategy_ic_result_cache.cpp:277-284`). It is refused only by `record.at("key")!=scope.key`
  (`:288-291`), with "candidate IC cache entry mismatch: volume_level". That comparison is exactly the
  S2-3 scope binding under test.
- **Teeth.** Without the key comparison the record would be served. Its values equal the true
  missing-exit-v1 IC by construction, so only the refusal can catch it. Restored, the run hits 2.

**(4) `IcLabelTerminal.FrozenTrainResumeBindsTheLabelRolePins`** (`:4681-4686`).
- **What it asserts.** The resumed `label_role_manifest_sha256` is exactly `{"validation": pin}`, and the
  resume with the TRAIN pin writes the same `recipe.json` SHA as the train-free resume.
- **Contract, not implementation.** It hard-codes no hash.
- **On pre-fix code.** Part 1 (4) showed that `7ccf81e8` already erased TRAIN before the pin loop. So
  this test passes on the pre-fix code too. It is a regression pin, and the new `Internal` refusal is
  unreachable defensive code.
- **Teeth.** If TRAIN ever entered the loop, the first `EXPECT` fails (`"train"` written), and the
  train-free resume hits the `Internal` refusal (`ASSERT` fails).

## B. Re-pin and receipts (open items 6-7)

See Evidence 2 and 3. Both are confirmed, with the one gap in Suggested 1.

## C. Item (8) and Ruling S2-5 (open item 8)

Characters per line (CRLF stripped), counted from git blobs:

| file | base `1239a5ff` | pre-fix `98c85cc9` | head | lane-added vs base | fix-added |
|---|---|---|---|---|---|
| `ic_research.hpp` | 1 | 1 | 1 | 0 | 0 |
| `ic_screen.cpp` | 14 | 18 | 17 | **5** | 0 |
| `ic_screen_test.cpp` | 23 | 28 | 23 | **0** | 0 |
| `strategy_ic_admission.cpp` | 76 | 91 | 90 | 14 | 0 |
| `strategy_ic_result_cache.cpp` | 25 | 26 | 26 | 3 | 0 |
| `strategy_ic_runner.cpp` | 119 | 139 | 139 | 22 | 0 |
| `strategy_ic_runner_test.cpp` | 487 | 544 | 544 | 59 | 0 |

- The engine test's over-length set at head equals base's, so the review's item 8 is done.
- `ic_screen.cpp` keeps exactly the 5 lines S2-5 lets stay: the min-coverage gate, the Bartlett SE / HAC
  pair, and the `prepare_ic_screen` call.
- The fix itself adds no line over 100 columns.

## D. Flag-absent identity (check d)

Part 1 (1.4) holds at head. Part 2 adds:
- **Cache keys.** The source digest is in no cache key. `ic_identity()` is semver `1` plus the
  build-flavour words (`strategy_ic_result_cache.cpp:57-62`). So every existing flag-absent IC-result
  entry still hits (no cold pass).
- **Flag-gated code.** Each of these runs only when `!cfg.label_terminal.empty()` / `terminal`, or
  takes the `MissingExitV1` early return at `ic_screen.cpp:323` (Part 1 cites `:324`):
  - `bind_label_role`, the label guard read and free, the `labels` recipe clause, the +4 B/cell term and
    the pin loop;
  - the new `terminal_inputs` checks.
- **Default path.** The `missing-exit-v1` IC beside the flag uses the default terminal
  (`strategy_ic_runner.cpp:406-407`). Under the default rule, `imputed` is never true, so the `||`
  leaves the old expression `bad[end]!=bad[entry]` in the same counter order.
- **Canary.** The log is green (15 passed against the base record), bound to the `80d81b26` exes by its
  timing.

## E. Look-ahead on the label path (check e, the reason for this round)

- **What the label guard reads.** It reads the label role only through
  `label_bad[exit]-label_bad[entry]`, with exit = T <= end. That is inside the label's own horizon,
  which the label already reads.
- **The guard is step-local.** `research_return_guard` (`role_panel.cpp:34-55`) needs both endpoints
  present. With the binding (close and raw_close bitwise equal, added presence only at `last+1`), the
  label prefix differs from the scored one only at the single step L -> T.
- **It can only drop a pair.** The guard can turn a pair's label into NaN, as missing-exit does. It
  never creates or changes a label value.
- **`last[i]`.** It scans the whole scored role (`strategy_ic_admission.cpp:429-432`). It decides only a
  refusal of the run, never a value.
- **Lifetime.** The label role and its guard are dropped before the scratch and any VM
  (`strategy_ic_runner.cpp:399-400`). Nothing reaches a signal, field, membership or the scored guard.
- **Fail-open closed.** Terminations-only closes the one fail-open edge the round targeted: the
  mis-imputation of a non-terminal exit at an interior gap (original #2).
- **Unchanged clock.** The 30-day cause-classification clock (original #3) is unchanged, and it is ruled
  to the prereg (S2-FIX1 (3)).

No look-ahead is introduced.

## F. Seal (check f, Ruling T2-SYN)

The fix round's lane commands were the two builds, the gtests, the canary and one pytest. Their inputs:
- **Engine fixtures:** date-index panels with no calendar.
- **Impl fixtures:** synthetic roles in temp dirs:
  - TRAIN sessions from day 17683 (2018-06-01) for 480 days;
  - validation sessions from day 18200 (2019-10-31).
- **Canary:** synthetic tiny_world. Its last session is 2022-12-30 (`scripts/tests/fixtures/tiny_world.py:63`),
  and the fixture refuses any world that reaches the seal (`:238-239`).
- **pytest:** the composition-rules fixture JSON.

Only the pre-existing `IcFieldsSeal` manifests name 2024-01-01, and they are synthetic seal fixtures
(T2-SYN). No real data was opened.

This reviewer opened only git blobs, the receipts and logs, and the pytest fixture. No 2020-2023 real
statistic was read: the canary's planted-member IC is synthetic.
