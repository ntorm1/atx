# Lane S2 re-review 1 (fix round 1) -- PARTIAL

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
