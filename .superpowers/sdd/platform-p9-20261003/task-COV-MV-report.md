# Lane COV-MV report

## Outcome

DONE_WITH_CONCERNS -- items 1-8 are implemented, committed and green in Debug on the committed head `846739c1`
(MvTarget 11/11, MvAimRule 15/15, the whole `atx-impl-strategy-target-tests` 405/405, teeth shown). Two things are
pending, because the permission system refused my command that reverts the teeth mutation (PERM-1: not retried,
not routed around):

- the working tree still carries the 2-line teeth mutation (uncommitted; never staged);
- the Debug tiny-world canary, and the one Release build with its suites and Release canary.

The refused command, exactly:

```
cd C:/atx-wt/pool-24 && L=.superpowers/sdd/platform-p9-20261003/covmv-logs; grep -n "RefusesAnIndustrySlotOutsideTheTable" -B 2 $L/e-teeth-refusals.log | head -3; awk '/RUN.*RefusesAnIndustrySlot/,/FAILED.*RefusesAnIndustrySlot/' $L/e-teeth-refusals.log | grep "error:" | cut -c1-200; git checkout -- atx-impl/src/strategy_nav_v7.cpp atx-impl/src/strategy_mv_aim.cpp && git status --short && grep -c "TEETH" atx-impl/src/strategy_nav_v7.cpp atx-impl/src/strategy_mv_aim.cpp
```

The rest of the lane does not depend on the revert, except the canary and Release builds. Those would compile the
mutated tree.

## Branch / SHA

- Branch `feat/p9-cov-20261003` in pool-24.
- Code and spec head `846739c1`. The report and handoff commit follows it; `git log -1` gives the final head.
- Root merged: first `1b64ce45` (merge commit `10259f38`, clean). At the end root had moved to `fd084e78` (SQL3's
  merge), and that head was merged too (merge commit `db33c6ca`, clean).
  - Root's range `1b64ce45..fd084e78` touches C++ only in `atx-engine` research-store `seal_guard.{hpp,cpp}` and its
    test; the rest is Python, specs and docs. None of it is a file of this lane.
  - The Debug evidence below is at `846739c1`, before that merge. Re-running the suites on the merged tree is
    pending with the revert.
- AL-CLOCK is NOT merged in root.
- Lease: untouched.

Commits, in order:

| SHA | what |
|---|---|
| `10259f38` | merge root `1b64ce45` (clean) |
| `756035ed` | feat(engine): mv-aim-v1 kernel (item 1) |
| `f6b05cbd` | feat(impl): the row, parser entry, hook, refusals, summary block, workspace term and impl gtests (items 2-7) |
| `26b67bd5` | test(cov): the a = 2 over-correction control (COV-11) |
| `d44e05f5` | fix(impl): an unused private field (slip at build `p9-covmv-b`) |
| `628a9ffb` | feat(specs): registration and template (item 8) |
| `846739c1` | test(impl): the calibrate test targets 1.1 x the plain gross (slip at build `p9-covmv-c`) |

## Working tree at hand-back (uncommitted; revert first)

These are the only dirty files:

- `atx-impl/src/strategy_nav_v7.cpp:1271`: `if (o.mv_aim && false) { // TEETH (reverted)`
- `atx-impl/src/strategy_mv_aim.cpp:135`: `if (true || slot < ...) continue; // TEETH`

The revert is `git checkout -- atx-impl/src/strategy_nav_v7.cpp atx-impl/src/strategy_mv_aim.cpp`. It needs the
owner's or the PM's approval, because that command was refused.

`build-equity/bin/atx-impl-strategy-target-tests.exe` is the teeth binary (tag `p9-covmv-e`). Rebuild it before
using it. `atx-equity-strategy-targets.exe` (`p9-covmv-d`) and `atx-engine-book-tests.exe` (`p9-covmv-a`) are
unmutated.

## Items -> change -> test -> evidence

| item | change (file:line at `846739c1`) | test | evidence |
|---|---|---|---|
| 1 kernel | `atx-engine/include/atx/engine/book/mv_target.hpp`, `atx-engine/src/book/mv_target.cpp` (`solve_pivoted` :98, `mv_aim` :155, `factor_exposure` :252, `factor_eigen` :272, `low_eigen_share` :304); one source line `atx-engine/CMakeLists.txt:174-175` | `MvTarget.*` 11 tests in `atx-engine/tests/book/book_mv_target_test.cpp` (globbed into `atx-engine-book-tests`) | `covmv-logs/a-mvtarget-debug.log`: 11/11 |
| 2 COV-13 (c) singular-safe form | push-through Woodbury: `(I + F G) y = F r`, `V^-1 a = u - D^-1 X y`; F never inverted | `MvTarget.RankDeficientFEqualsDenseSolve` (F = v v' with a zero row: finite, equals the dense solve to 1e-12; F = 0 too), `.ThreeNamesDiagonalClosedForm` (F = [0], exact) | same log |
| 3 PRE-9 row, append-only | `atx-impl/src/book/nav_rules.cpp:97-118` (target table; `rules_json` wraps it as a span at :191); `nav_rules.hpp:16-17` comment | `MvAimRule.ListRulesRowIsAClosedTargetRow`; the `--list-rules` diff against root's p9-2p exe (below) | `covmv-logs/d-mvaimrule-debug.log`, `d-list-rules-diff.log` |
| 4 parser entry, hook, refusals | `strategy_nav_v7.cpp:1145-1149` (handler), `:1265-1299` (refusals), `:1303` (store flags let through), `:1437` (store opened); `book/nav_decide.cpp:42-48` (the hook); `strategy_mv_aim.cpp:127` (read row d), `:132-140` (S11) | `MvAimRule.RefusesWithoutAStore`, `.RefusesIncompatibleRules`, `.RefusesAnIndustrySlotOutsideTheTable`; teeth: all three fail with the refusals disabled | `d-mvaimrule-debug.log`; teeth `e-teeth-refusals.log` (0/3 pass, 35 failed assertions) |
| 5 print-only `mv_aim` block | `strategy_mv_aim.cpp:197-271` (`record`, `parameters_json`, `summary_json`); `strategy_nav_v7.cpp:320-328` (`add_mv_aim`), :607, :1001, :1029 | `MvAimRule.SummaryBlockOnlyWhenOn`, `.DecisionCountedOnceWarmUpAndLocateNegatives`, `.FewerThanTwoPricedPassesThrough`, `.CalibrateGrossCountsEachDecisionOnce` | `d-mvaimrule-debug.log` |
| 6 flag-absent identity; workspace term (COVMV-1) | hook identity without the rule (`strategy_nav_v7.cpp:948-958`); ONE term at `book/nav_lockstep.cpp:74-75` | `MvAimRule.FlagAbsentIdentity`, `.WorkspaceReserveTermOnlyWithTheRule`; `SummaryBlockOnlyWhenOn` (off run: no file holds `mv_aim` / `mv-aim-v1`) | `d-mvaimrule-debug.log`; whole suite `d-whole-target-tests-debug.log` 405/405 |
| 7 tests on synthetic containers | `atx-impl/tests/strategy_mv_aim_rule_test.cpp` (15 tests; directory and container stores) | `MvAimRule.*` | as above |
| COV-11 | the a = 2 control `atx-impl/tests/strategy_cov_model_test.cpp:647-657`; the per-row group check `strategy_mv_aim.cpp:132-140` | `CovBias.EigenFamilyInBandOnAPlantedModel`: a = 2 misses the band, pooled 0.957 +- 0.025, 5/7 in band | `covmv-logs/c-named-debug.log` (99/99) |
| 8 registration and template | `scripts/specs/p9/rules/mv-aim-v1.json`, `scripts/specs/p9/templates/mv-aim.json` | `validate_specs.py` (the repo's own validators) | `covmv-logs/d-validate-specs.log` |

Optional items done: the special-tier negative count, the warm-up count, a decision counted once across a second
pass, and `--calibrate-gross` compatibility with its test.

## Evidence (Debug, equity-dev; build tags and receipts in `build-equity/mega-p9-covmv-*-receipt.json`)

| tag | targets | source / dirty | exit | exes (SHA-256 prefix) |
|---|---|---|---|---|
| p9-covmv-a | atx-engine-book-tests | `756035ed` / 0 | 0 | book-tests `95a48109ffc84faa` |
| p9-covmv-b | target-tests, targets | `26b67bd5` / 0 | 1 | -- (slip: `-Wunused-private-field`, fixed `d44e05f5`) |
| p9-covmv-c | target-tests, targets | `d44e05f5` / 0 | 0 | tests `5c455d6e4c081fad`, targets `5d0a85fc2f7f7e2d` |
| p9-covmv-d | target-tests, targets | `846739c1` / 0 | 0 | tests `58d7bcb2444a8b58`, targets `75593d6fb8726797` |
| p9-covmv-e | target-tests (TEETH, mutated) | `846739c1` / 2 | 0 | tests `ed30f4e0df422915` (teeth only) |

All logs are in `.superpowers/sdd/platform-p9-20261003/covmv-logs/`:

- `build-equity/bin/atx-engine-book-tests.exe --gtest_filter=MvTarget.*` -> exit 0, `[  PASSED  ] 11 tests.`
  (`a-mvtarget-debug.log`).
- `atx-impl-strategy-target-tests.exe --gtest_filter=MvAimRule.*` (p9-covmv-d) -> exit 0, `[  PASSED  ] 15 tests.`
  (`d-mvaimrule-debug.log`).
- The same filter on `p9-covmv-c` failed one test: `CalibrateGrossCountsEachDecisionOnce`. Its fixed `--calibrate-gross
  1.2` put L' at 168.9, outside [1, 2], because the fixture's S2 gross is about .0085. The test was fixed to
  1.1 x the plain run's gross (`846739c1`). This was a test slip; no code changed. Log: `c-mvaimrule-debug.log`.
- `--gtest_filter=Cov*:NavBookRule.*:VolTarget.*:Spo*:NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*:NavAnnMeanGross.*`
  (p9-covmv-c; c -> d changed only the MvAimRule test file) -> exit 0, `[  PASSED  ] 99 tests.` from 22 suites
  (`c-named-debug.log`).
- The whole `atx-impl-strategy-target-tests.exe` (p9-covmv-d) -> exit 0, `[  PASSED  ] 405 tests.` with 1 DISABLED
  (COVR's `DISABLED_` stage diagnostic, by design) (`d-whole-target-tests-debug.log`).
- Teeth (p9-covmv-e: the parser's mv-aim-v1 refusal block and the S11 check disabled at once):
  `--gtest_filter=MvAimRule.Refuses*` -> exit 1, `[  PASSED  ] 0 tests.`, 3 FAILED, 35 failed assertions
  (`e-teeth-refusals.log`). With the refusals disabled:
  - `--rule spo-v1` with the rule published a run (exit 0);
  - spo-v2 failed on its own gamma calibration;
  - a slot 60 at row 25 was accepted.
- `atx-equity-strategy-targets.exe nav --list-rules --json` (p9-covmv-d), compared with root's p9-2p exe (SHA-256
  `637f7069...8827`, source `327c86b2`, verified against its receipt) -> `schema/caps equal: True True`, `old rows a
  prefix of new: True`, `added: [('mv-aim-v1', 'target')]` (`d-list-rules-diff.log`). `--list-rules` was the only
  thing run on root's exe; it reads no data.
- `PYTHONDONTWRITEBYTECODE=1 python covmv-logs/validate_specs.py covmv-logs/d-nav-list-rules.json` -> exit 0
  (`d-validate-specs.log`):
  - `rule_cell_problems: []`, `registration_problems: []`;
  - the template SHA-256 equals the pin, with 0 CR bytes;
  - `load_spec: p9-mv-aim | nav.output build-equity/mega-nav-p9-mv-aim | monitor.output
    build-equity/mega-monitor-p9-mv-aim`;
  - `rule_flags problems: []`; derived flags `{"nav": {"--risk-model": "build-equity/p9-m2i-dcov-covv1",
    "--risk-model-sha256": "ea58691f...f0f3", "--target-rule": "mv-aim-v1"}}`.
- Python: no Python file changed. No Python test reads `scripts/specs/p9/rules/` or `templates/` (grep: only
  `test_p9_al_sig_registrations.py` and `test_wave_kinds.py` touch `scripts/specs/p9`, and only `candidates/`,
  `blocked/` and `kinds/`). So `scripts/tests` seeds 0 / 1 and the stage chain are not required, and were not run.
- NOT run: the tiny-world canary (Debug or Release) and every Release build or suite. Both are pending the revert;
  see "Pending".

## Flag-absent identity procedure for root

- **Bytes.** The rule is absent unless `--target-rule mv-aim-v1` is given. Root's procedure:
  - Run X-5's and Y-F0's NAV argv on the new build and on the pre-merge exe.
  - Every NAV file should be byte-identical. The substitution list, written before the run: `summary.json`
    `/producer` only (C1-PROD).
  - Without the rule the hook returns before touching the target (`strategy_nav_v7.cpp:948-952`). No key and no rule
    suffix is written (`add_mv_aim` returns at :324). The workspace reserve is unchanged
    (`extension_workspace_bytes` is 0, :955-958).
- **In-binary.**
  - `MvAimRule.FlagAbsentIdentity`: the hook is the identity without an extension and with an extension that lacks
    the rule. Replay days are bit-equal with and without an extension that has no rule, and the rule moves them
    (teeth).
  - `MvAimRule.WorkspaceReserveTermOnlyWithTheRule`: `nav_workspace_reserve_bytes` is unchanged without the rule,
    and is `+ mv_aim::workspace_bytes(names)` with it.
  - `SummaryBlockOnlyWhenOn`: an off run holds no `mv_aim` / `mv-aim-v1` byte in any file.
- **Rule table.** `nav --list-rules --json`, new vs pre-merge: the `mv-aim-v1` row is the only addition (shown above
  against p9-2p).
- **Help.** `nav --help` changes, help bytes only: the `--target-rule` sentence (`strategy_nav_v7.cpp:1039-1043`) no
  longer says "none registered in this build". The header comment `strategy_nav_v7.hpp:15-21` changes the same way.
- **Canary.** The tiny-world canary goldens, Debug and Release, are pending (see "Pending").

## What root re-proves at merge

1. The equity-dev build of `atx-engine-book-tests`, `atx-impl-strategy-target-tests` and `atx-equity-strategy-targets`
   has 0 warnings. Then equity-rel. Filters, Debug and Release:
   - `atx-engine-book-tests --gtest_filter=MvTarget.*`;
   - `atx-impl-strategy-target-tests --gtest_filter=MvAimRule.*:Cov*:NavBookRule.*:VolTarget.*:Spo*:NavSpec.*:NavRules.*:CostLawRegistry.*:TradeRateRegistry.*:TargetRegistry.*`;
   - the whole target-tests.
2. The rule-absent identity (above) on real data, 0 trials. X-5 and Y-F0 NAV byte-identical; `/producer`
   substituted; Release equals Debug on X-5.
3. The `--list-rules` diff: one added row.
4. The tiny-world canary goldens, Debug and Release.
5. No run of the rule on TRAIN or any real role before P9-MV is registered (COV-2).
6. Before P9-MV:
   - `universe.delisting.returns_applied == false` (S9, COV-9);
   - the lineage parent re-pinned (W1-I2);
   - SQL2-CLS: no new `atx.<name>/v<n>` literal is introduced (the summary key `mv_aim` is a block, not a schema).

## For PRE: the P9-MV block

- **Rule / row.**
  - `mv-aim-v1`, kind `target`, x-flag `--target-rule mv-aim-v1`.
  - params: `risk_model` (`--risk-model`) and `risk_model_sha256` (`--risk-model-sha256`, pattern
    `^[0-9a-f]{64}$`), both required (PRE-9). The schema is closed.
  - incompatible: `aim-partial-v6, spo-v1, spo-v2, spo-v3, two-speed-nav-v1, adv-hold-v1, hold-band-v1`.
  - The NAV exe also refuses a `--rule` other than `aim-partial-v5`, and `--construction-grid`.
- **Registration.** `scripts/specs/p9/rules/mv-aim-v1.json`, SHA-256
  `4b4af6e238b529d5d7eabccd2699022dc653da52fa45801eb3f61896bd6680b4`.
- **Template.** `scripts/specs/p9/templates/mv-aim.json`, SHA-256
  `83391f2c526b28008d48e250f0e7d26e7c3f8661786d43a8f3d76439f664d870` (LF bytes; the registration pins it).
  NAV-only: the nav and monitor outputs are renamed. Nominal parent `../../v8/lib-v8ysb-gm.json` (Y-F0); parent null,
  root sets it.
- **Manifest fragment.**
  - `gross_match: "pm6-6-engine"`;
  - `rules: [{name: mv-aim-v1, params: {risk_model: build-equity/p9-m2i-dcov-covv1, risk_model_sha256:
    ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3}}]`;
  - E2's `rule_flags` derives exactly `--risk-model`, `--risk-model-sha256` and `--target-rule mv-aim-v1` from the
    built exe's row.
  - The store pin is named only: the lane never opened `build-equity/p9-m2i-dcov-covv1` (W3-10, PRE-29).
- **`source_sample_end`: 2010, verified.** Primary source: Menchero, Wang and Orr (2011), "Eigen-Adjusted Covariance
  Matrices" (cov-design section 8 source [2]). Read on 2026-10-06 in the paper's text:
  - "we select the 50 largest US stocks as of July 30, 2010 that have full daily return histories dating back to
    July 1, 1993";
  - "the testing window is 4,103 trading days (from April 15, 1994 to July 30, 2010)".
  - The year is before 2015, so it is NOT flagged under PRE-19.
- **K-P9-11 keys.**
  - `predicted_mechanism`: "V^{-1} tilt lowers ex-ante and realised vol at equal gross alpha";
  - `data_class`: `H`.
- **C3-4 realised gross.** The rule has no leverage parameter. It holds the target's gross at the desired target's
  (W3-17), so the book's realised gross leverage is aim-partial-v5's at the parent's L, or the L' of the PM6-6 match.
  It cannot exceed a parameter of its own. Under a leverage rule the cap is that rule's; P9-MV names none.
- **Printed figures and producers.** All come from `atx-equity-strategy-targets nav`, `summary.json` key `mv_aim`,
  main pass (`strategy_mv_aim.cpp:238-271`):
  - `decisions {formed, applied, pass_through {no_forecast, fewer_than_two_priced, flat}}`, `warm_up_decisions`;
  - `priced_names` / `unpriced_members` {n, mean, min, max};
  - `specific_vol_ratio {pooled, per_decision}`: PRE-24a, sum |w| sigma / sum |desired| sigma over the priced
    names;
  - `eigen_00_02_share {mv, desired}`: COV-13 (c), the share of e' F_d e on F_d's three smallest eigenvalues over
    the forecast block, e = X'w;
  - `special_tier_negative {locate_in_aim, name_decisions, decisions}`.
  - `recipe.json`, the holdings manifest and `capacity/summary.json` carry `{rule, declaration, risk_model}` only.
  - Cost per traded dollar and turnover come from the existing NAV summary and capacity curve, as for every cell.
- **No constant.** There is no IC, horizon, ridge, cap, clip or floor. `FactorModel::create`'s D floor is not used: a
  name without a finite D_i > 0 is unpriced and keeps its weight.

## What the reviewer traces for COV-4

The decision at session t is formed and first traded as follows (code at `846739c1`):

1. `book/nav_session.cpp:324`: session t runs MARK, EXECUTE (`s.execution = t > start && t + 2 <= end`), then DECIDE
   (`s.decision = t + 2 < end`). See `strategy_nav_replay.hpp:23-30`: decide at session d after its mark, fill at
   d + 1's close, first return row d + 2.
2. `nav_session.cpp:348` calls `decide_construction(x, fields, base, t, ...)` on the calling thread, before the
   books' `for_each_lane`.
3. That reaches `book/nav_decide.cpp:42-47`: `form_desired` builds the target from rows <= t, then
   `v7::apply_target_rule(x, d = t, ...)`.
4. That reaches `strategy_mv_aim.cpp:108` (`check_axes`: the store's sessions are the replay's) and :127
   (`store_->read(d, slice_)`, row d only). This is the same index as `BookScaler::estimate`
   (`strategy_risk_target.cpp:141`) and spo (`strategy_spo.cpp:1156`). No other row, no `at_or_before` and no
   whole-axis statistic of the store is read; sigma_i comes from the same row.
5. `nav_session.cpp:350`: the calibrate tape records the post-rule target, so a restored pass replays it.
6. Row d's stamp is as_of = cutoff = d (K-P9-12; COV-4: block t uses sessions <= t). The decision at d is traded
   first at d + 1's close.
7. Tests:
   - `MvAimRule.TargetAtDReadsStoreRowDOnly`: every store row after T is replaced, and the targets at d <= T and
     the NAV rows through T stay bit-equal. Teeth: replacing row T moves the target at T, and later rows move.
   - `MvAimRule.FutureReturnDoesNotChangePast`: the role and the store are cut after t. The targets at d <= t and
     the rows through t - 2 (the cut run's last decision) are bit-equal.

## Cross-lane edits (file and hunk)

- `atx-engine/CMakeLists.txt` @@ -173,0 +174,2: one source line `src/book/mv_target.cpp` (and its comment) after
  `src/book/risk_target.cpp`.
- `atx-impl/CMakeLists.txt` @@ -301,0 +302,3: tail append `target_sources(atx-impl-strategy PRIVATE
  src/strategy_mv_aim.cpp)` (COVMV-1).
- `atx-impl/tests/CMakeLists.txt` @@ -209,0 +210,3: tail append `target_sources(atx-impl-strategy-target-tests
  PRIVATE strategy_mv_aim_rule_test.cpp)`.
- `atx-impl/src/book/nav_rules.cpp` (C3):
  - @@ -6,0 +7: one include;
  - @@ -96,3 +97,22: the target table replaces the empty slot;
  - @@ -171 +191: `rules_json` wraps `std::span<const Row>(targets)`.
- `atx-impl/src/book/nav_rules.hpp` (C3) @@ -16,2 +16,2: the comment for the target kind.
- `atx-impl/src/book/nav_decide.cpp` (C3) @@ -42,2 +42,7: the hook (ATX_TRY on `form_desired`, then
  `apply_target_rule`).
- `atx-impl/src/book/nav_lockstep.cpp` (C3) @@ -74 +74,2: ONE added term `+ v7::extension_workspace_bytes(names)`
  (COVMV-1).
- `atx-impl/src/strategy_nav_v7.{hpp,cpp}` (L4 / C3 parser):
  - the option, State member, begin_run, accessor;
  - the handler at :1145-1149, which replaces C3's "no handler" path for this id only;
  - the refusal block at :1265-1299, the store flags at :1303 and the store open at :1437;
  - `add_mv_aim` and its three calls;
  - `apply_target_rule` and `extension_workspace_bytes`;
  - help and header text.
- `atx-impl/tests/strategy_cov_model_test.cpp` (COV) @@ -646,0 +647,11: the a = 2 control (COV-11).

**W3-9 keep-both note (AL-CLOCK).**

- AL-CLOCK (pool-8, head `c3f3f1f8`, WIP) is not merged in root.
- `git merge-tree --write-tree HEAD c3f3f1f8` gives a clean tree with no conflict. AL-CLOCK appends to the
  trade-rate table and to the `(...)` sum inside `nav_workspace_reserve_bytes`. This lane replaces the target slot
  and appends a term after the last line of that return. Both hunks are kept.
- Root resolves the keep-both at the second merge if a later AL-CLOCK head conflicts.

## New literals, flags, keys, rule ids

- Rule id `mv-aim-v1` (9 characters; W1-I1 <= 18 holds).
- The flag value `--target-rule mv-aim-v1`. `--target-rule` itself is C3's.
- JSON key `mv_aim`, in `summary.json`, `recipe.json`, the holdings manifest and `capacity/summary.json`.
- The rule-id suffix `+mv-aim-v1`. No Python parses the suffix.
- No new `atx.<name>/v<n>` schema literal.

## Deviations, design notes, risks

- The brief named `risk::FactorModel` / `apply_inverse`. COV-13 (c) forbids inverting F, so the kernel uses its own
  push-through form, as the handoff fixed. It is defined for a singular F. A non-PSD F (zero pivot) refuses the
  run.
- The registered form is P V^-1 P alpha, gross-normalised. It is not the constrained optimum V^-1 (alpha - c 1) of a
  dollar-neutral max-Sharpe problem. This is a design note, not a stop.
- The incompatible list goes beyond the spo rules:
  - two-speed-v1, adv-hold-v1 and hold-band-v1 shape the target or split it per book;
  - a grid's variants could disagree on the rule's own target;
  - aim-partial-v6 reshapes the target by cost.
  - Each has a refusal test with teeth.
- `--adv-hold-q` is refused when present, even at value 0. Y-F0's NAV flags carry none.
- `--locate-in-aim` is allowed. A negative target on a special-tier name is not clipped (no extra constant); it is
  counted. The books' locate block still applies at the plan.
- The eigen print raises an error if F_d's forecast block is not symmetric (`symmetric_eig`, relative 1e-9). It is a
  store defect, never silent. The figure is print-only.
- Debug cost: the eigen print runs Eigen's solver once per applied, scored decision on 62 x 62. This is negligible
  in Release; Debug runs of the rule on a full role are slower.
- The decide path (`strategy_live.cpp:1377`) installs no extension, so the hook is the identity there.

## Pending (hand-back; see `handoff-COV-MV.md` "Session R8")

1. The owner or the PM approves the revert of the teeth mutation: `git checkout -- atx-impl/src/strategy_nav_v7.cpp
   atx-impl/src/strategy_mv_aim.cpp`.
2. Debug canary: `research-build -Tag p9-covmv-f -Targets atx-equity-strategy-ic,atx-equity-strategy-targets,
   atx-research-eval,atx-impl-strategy-target-tests -Canary`.
3. Release: configure `equity-rel`, then build `p9-covmv-g` with `atx-engine-book-tests`,
   `atx-impl-strategy-target-tests`, `atx-equity-strategy-ic`, `atx-equity-strategy-targets` and `atx-research-eval`
   `-Canary`. Then the Release filters and the whole target-tests.
4. Re-run the Debug filters on the merged tree (root `fd084e78`, merge `db33c6ca`). Merge root again if it moved
   after that.
5. Delete `build-equity` and `build-equity-rel` by name (keep the receipts and logs).

## Disk

- Freed: 0. `build-equity` (2.9 GB) is kept warm for the pending Debug canary build. C: had 50 GB free.

## Open questions (one line each)

- May the teeth mutation be reverted with the refused `git checkout --` command (or by an edit), so the canary and
  Release can run?

## Ledger candidates

- `git checkout -- <file>` to revert a teeth mutation can be refused by the permission system. Next time, mutate a
  copy-free way that reverts by re-applying a recorded patch, or ask before the mutation.
- In the spo fixture (Role 40 x 12, NAV 1e8), the S2 book's all-rows gross is about .0085 (participation caps). A
  fixed `--calibrate-gross` target leaves L' outside [1, 2]; derive it from a plain run.
