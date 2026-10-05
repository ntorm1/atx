# Lane S2 report (P9 wave 2, slot 3): VM kernels in worker bands, one OpSig table, IC label / coverage / HAC options

## Outcome

DONE_WITH_CONCERNS (concerns are root-side checks a lane may not run; nothing known red).

- Pool `C:/atx-wt/pool-18`, branch `feat/p9-s2-20261003`, base `1239a5ff`.
- Commits (base..head): `cc882481` wip (op traits, banded kernels, IC options, label terminal, seal
  consumer; written before the owner stop), `eff81c86` handoff, `bbc7da8d` fix (imputed terminal
  on the end session), `99ba1a64` feat (S2-3 own scope, S2-2 capabilities, S2-1 re-pins),
  `23533f54` engine tests, `4cb9265a` runner tests, then this report's commit.
- Resumed from `handoff-S2.md` (~65%); rulings applied: RESUME, W2-BUILD, DISK / DISK-2, S2-1,
  S2-2, S2-3.

## Brief / carry items

| item | became |
|---|---|
| one constexpr per-opcode table (family, axis, needs_group, stateful), `static_assert` size == last opcode + 1, every predicate derived, `lookahead_safe` derived | `alpha/op_traits.hpp` (`OpTraits` / `kOpTraits`, exhaustive `op_traits_row`, consistency `static_assert`); typecheck, VM, StreamingEngine, oracle and `Library` read it. `OpSig.PredicatesAgree` pins every derived predicate against the pre-S2 hand lists for every opcode. |
| band-split of the serial kernels by `chunk_axis` with per-worker scratch | `vm.hpp` `dispatch_banded` / `run_bands` / `run_band` + `BandScratch` per worker (LoadField, Const, element-wise, W2 map / Cs / Ts, as-of, recurrences, Split2, KalmanReg). Pin and GroupDelay stay unbanded. |
| columns extracted once per instruction in `eval_lit_ts` | per band, each input column of instrument j extracted once into `BandScratch::lit_col`; a cell's window is a slice (unary) or n_in slices copied (multi-input). |
| `IcScreenConfig::min_coverage` (default .8) keyed in the IC-result scope | engine field + validation (in (0, 1]) + `ic_screen_cache_matches`; runner `--min-coverage F` (recipe key and scope key `min_coverage_bits` only when non-default). |
| `--label-terminal imputed-v1` (label-only price column, imputed terminal; missing-exit share; IC under both rules) | `LabelTerminalRule` / `ResearchIcTerminal` (engine), `--label-terminal imputed-v1` + `--train-label-role[-sha256]` / `--validation-label-role[-sha256]` (runner); per-candidate and blend `ic_missing_exit_v1`, coverage `terminal_imputed_pairs` / `missing_exit_share`, role block `label_terminal`, recipe `label_terminal` / `labels` / `label_role_manifest_sha256`, frozen-TRAIN pin binding. |
| `--eval-mode audit-exact` with the cache under its own identity token | `vm_identity(true)` = identity + `_audit` (own cache root), `CacheKey::eval_mode` keyed / matched / written; the old refusal with `--candidate-cache` is removed. |
| IC runner HAC rule named and selectable beside `eval/hac.hpp`'s (default unchanged) | `IcHacRule {Bartlett2hV1 (default), HorizonAwareV3}`, `ic_hac_rule_name`, `estimate_daily_ic`; runner `--ic-hac-rule` (recorded when given; non-default keys scope `hac_rule`; every estimate prints a `hac` block with both rules' SE and lag). |
| P16: survival-label source = role builder's imputed close; scope adds `src/alpha/*.cpp` + IC runner / cache / admission files | label role must declare `universe.delisting.returns_applied` true (E-10 relaxed for this label-only column, never for a signal or field), share dates / instruments / score window / sessions / ids, and equal the scored role bit for bit on every scored-present cell (`bind_label_role`); read only for labels, dropped before any VM. |
| P13: C++ seal consumer, IC half | `fields_seal` in admission `bind_fields`: `seal.exclusive_end` == this build's `kSealBeginDate` binds; no seal / no `exclusive_end` = legacy, accepted and logged per role (`IC fields-seal role=<r> seal=absent accepted-as-legacy-manifest`); another seal or a malformed block refuses before payload. Also reaches `strategy_research_role.cpp` (shares `bind_fields`), as P13 intends. |
| S1-PIN: S2 owns `ic_screen.{hpp,cpp}` | respected. |
| Ruling S2-1 | both digests re-pinned WITHOUT a semantics bump: `dsl_vm_sources_sha256` `fcf8021e...` -> `5bcd55c78814451391063cd9278852f765f02b5378a04d7598c188a568f712b2` (list 34 -> 35, `op_traits.hpp` joins the closure); `ic_result_sources_sha256` `2d758bff...` -> `bd13574c08a61e4c4ac92c351a84dc1face7e2910dd7c8a784f2f278adb29317`. Digest recomputed by a scratch script that reproduces both old pins from the base blobs exactly, and re-checked on the committed HEAD blobs; both tripwire gtests green. No expected output hash was edited. |
| Ruling S2-2 | `exe_capabilities` gains `ic-hac-rule`, `label-terminal`, `min-coverage` (sorted, existing entries unchanged); the K-P9-6 fixture `composition_rules_list.json` regenerated to match; `ListRulesJsonPinned` also asserts the three. |
| Ruling S2-3 | the missing-exit-v1 IC beside `--label-terminal` is cached under its own IC-result scope (`label_terminal: "missing-exit-v1"` in the key, no label pin), distinct from both the imputed scope and the flag-absent scope; `--no-composition` skips a signal load only when both IC results are cached. |

Contracts: K-P9-6 (`--list-rules --json` envelope) capabilities extended per S2-2; no other K-P9 contract is S2's.

## Bug found and fixed while testing

`IcLabels.TerminalImputedClosedForm` (h = 1) found that `ImputedV1` chose the exit price with
`exit == end`, so a name whose termination session is the horizon's own end session read the
scored role's absent (NaN) close and was counted `invalid_price_pairs` instead of imputed. Fixed
with an explicit `imputed` flag (`bbc7da8d`).

## Files changed

Owned (brief + P16 + S1-PIN): `atx-engine/include/atx/engine/alpha/{op_traits.hpp (new),registry.hpp,
streaming_engine.hpp,typecheck.hpp,vm.hpp}`, `atx-engine/src/alpha/{oracle,registry,typecheck}.cpp`,
`atx-engine/include/atx/engine/factory/{ic_screen.hpp,ic_screen_config.hpp}`,
`atx-engine/src/factory/ic_screen.cpp`, `atx-impl/src/strategy_ic_{runner.cpp,runner.hpp,detail.hpp,
admission.cpp,signal_cache.cpp,result_cache.cpp}`; tests `atx-engine/tests/alpha/alpha_op_traits_test.cpp`
(new), `alpha_vm_slot_reuse_test.cpp`, `atx-engine/tests/factory/ic_screen_test.cpp`,
`atx-impl/tests/strategy_ic_runner_test.cpp`.

Beyond the literal list / cross-lane (listed for root):
- `atx-engine/include/atx/engine/factory/ic_research.hpp`: the research-IC API whose definitions live
  in `ic_screen.cpp` (S1-PIN file); needed for `LabelTerminalRule` / `ResearchIcTerminal`.
- `atx-impl/src/strategy_ic_rules.cpp` (`exe_capabilities` only), `atx-impl/tests/fixtures/composition_rules_list.json`
  (3 capability lines), `atx-impl/tests/strategy_ic_rules_test.cpp` (one assertion list): ruling S2-2,
  D-lane files. Overlap risk with D2 (pool 20): textual only.
- No CMake edit (engine test groups glob; `alpha_op_traits_test.cpp` joins `atx-engine-alpha-tests`).
- No new `atx.<name>/v<n>` schema literal (SQL2-CLS: nothing for `classes.json`).

## Tests written (gtest names)

- `OpSig.PredicatesAgree`, `OpSig.TableRowsFollowTheirFamily`, `OpSig.LookaheadSafeDerivedFromTheTable`.
- `alpha_vm_slot_reuse_test.cpp`: `formulaic_ops()` rows join `catalogue_forms` (the Catalogue test's
  row count now includes them: a scope change, not an expected-value edit) and the Catalogue test
  asserts every non-housekeeping opcode is run by some variant; new
  `AlphaVmBandParity.EveryOp_Workers1_4_16{AuditExact,ResearchFast,MaskedAuditExact,MaskedResearchFast}_ByteEqualToSerial`
  (Cs and Ts pools = one DetPool of 1 / 4 / 16 workers, as the IC runner sets them, vs the pool-less
  Engine, every catalogue variant) and
  `AlphaVmTruncation.EveryOp_PrefixPanel{...}_ByteEqualToFullPanelRows` (prefix panels of 5 and 10 of
  12 dates, rows byte-equal). This sweep is the "worker parity 1 / 4 / 16 for every banded kernel"
  and "per-opcode truncation-invariance and slot-reuse sweep" of the brief: it covers every banded
  kernel and every other kernel without a hand list (the handoff's separate `vm_band_parity_test.cpp`
  was folded into it; see Deviations).
- `IcScreen.MinCoverageConfig`, `IcScreen.HacRuleNamedSelectableAndReproduced` (HorizonAwareV3 SE ==
  `mean_tstat` x T/n bit for bit; Bartlett default == the pre-S2 arithmetic bit for bit;
  `estimate_daily_ic` reproduces both evaluated estimates), `IcLabels.TerminalImputedClosedForm`.
- Runner: `StrategyIcRunner.AuditExactEvalModeIsRecordedAndCachedUnderItsOwnIdentity` (replaces
  `...AndRefusedWithTheCache`: the old expectation encoded the refusal the brief removes; the CLI half
  kept), `IcMethodOptions.RecordedOnlyWhenGivenPrintBothHacRulesAndKeyTheIcCache`,
  `IcLabelTerminal.ImputedRunPrintsBothIcsCountsImputedPairsAndRecordsThePin` (imputed pairs 5 / 21 / 33
  in closed form; `ic_missing_exit_v1` == a plain run's IC),
  `IcLabelTerminal.LabelRoleRefusals`, `IcLabelTerminal.FrozenTrainResumeBindsTheLabelRolePins`,
  `IcLabelTerminal.MissingExitIcIsCachedUnderItsOwnScope` (S2-3), `IcFieldsSeal.ManifestSealMustBeThisBuildsOrAbsent` (P13).

## Evidence (W2-BUILD: lane builds through `scripts/research-build.ps1`, equity-dev, memory-gated)

Configure: `atx-build.ps1 configure -Preset equity-dev` (pool-18 had no equity tree). Build tags
(receipts `build-equity/mega-p9-s2-<tag>-receipt.json`):

| tag | source | targets | exit |
|---|---|---|---|
| a | eff81c86 | atx-engine-ic-screen-tests | 0 (93/93 pass at that point) |
| b | eff81c86+wip | atx-equity-strategy-ic, atx-impl-strategy-ic-tests | 0 |
| c | wip | + atx-engine-alpha-tests | 1 (two test-only `run_named(temporary)` compile errors, fixed; alpha tests linked) |
| d, e | wip | atx-impl-strategy-ic-tests (+ ic exe) | 0 |
| base | 1239a5ff | atx-equity-strategy-ic, atx-equity-strategy-targets | 0 |
| head | 4cb9265a | atx-equity-strategy-ic, atx-equity-strategy-targets, atx-impl-strategy-mine-tests | 0 (DirtyEntries 1: the goldens file during the identity check, restored after) |

Test runs (all on the equity-dev Debug tree, head sources):
- `atx-impl-strategy-ic-tests.exe` (whole binary, includes `ic_screen_test.cpp`, `ic_research_test.cpp`, rules, runner): **202 / 202 passed** (both source tripwires green after the re-pins).
- `atx-engine-alpha-tests.exe` (whole group): **782 / 782 passed** (root's M1d count 771 + the 11 new).
- `atx-impl-strategy-mine-tests.exe`: **44 / 44 passed**, incl. `SignalFitnessDefaults.ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` (golden `0x889874a3b9b29c55` at 1 and 4 workers) and `...ImplicitDefaults...`.
- pytest (PY-HYG, explicit paths): `atx-impl/tools/test_fit_composition_rule_plugins.py` -> `8 passed` (exit 0, reads the regenerated fixture); `scripts/tests/test_cycle_e2e.py` with `ATX_EQUITY_BIN=<pool-18>/build-equity/bin ATX_EQUITY_BUILD_TYPE=Debug ATX_CANARY_REQUIRED=1` -> `15 passed in 12.84s` (exit 0; the identity check below).

### Flag-absent byte identity (lane-runnable, synthetic only)

The tiny_world canary (synthetic, every session before the seal) through the real exes, every S2
flag absent: digests recorded with the BASE exes (`1239a5ff`, ic `c246d4f4...`, nav `73774e3c...`)
by `test_cycle_e2e.py --record --build-type Debug`, then the HEAD exes (`4cb9265a`, ic `b2b663e1...`,
targets `7f515c95...`) checked against them with the canary pytest: **equal** for
`u_orientations_json_sha256` `38115194...`, `fit_admission_decisions_sha256` `71b47e87...`,
`fit_weights_sha256` `d222c876...`, `w_combined_f64_sha256` `1f7ac0a8...`,
`nav_primary_daily_csv_sha256` `ca559404...` (primary scenario `modeled-1bn-stale5-v1+swap-fin-v1`),
plus the second idempotent run. Evidence: `s2-evidence/canary-base-1239a5ff-debug-record.json`,
`s2-evidence/canary-head-4cb9265a-debug-check.log`. The tracked goldens file was restored to its
committed bytes afterwards (Debug / Release stay null for root to record after C1, per T1).

## How root verifies

- Build (target-scoped): `atx-engine-alpha-tests`, `atx-engine-factory-tests` (or the bounded
  `atx-engine-ic-screen-tests`), `atx-impl-strategy-ic-tests`, `atx-impl-strategy-mine-tests`,
  `atx-equity-strategy-ic`, `atx-equity-strategy-targets`; Debug and Release.
- Anchored gtest filters: `OpSig.*`, `AlphaVmSlotReuse.*`, `AlphaVmBandParity.*`, `AlphaVmTruncation.*`,
  `IcScreen.*`, `IcLabels.*`, `ResearchIc.*`, `StrategyIcRunner.*`, `IcMethodOptions.*`,
  `IcLabelTerminal.*`, `IcFieldsSeal.*`, `CompositionRules.*`, `NoComposition.*`, `Workers.*`,
  `SignalFitnessDefaults.*` (golden at 1 and 4 workers), plus the whole alpha / factory groups.
- Identity runs root must do (lanes may not): X-5 u and w byte-identical with every S2 flag absent
  (recipe, orientations, daily IC, candidates, summary minus timings, IC cache keys and the existing
  IC-result entries hit); cold u wall before / after (the banding should make a pooled cold u faster,
  never slower; E-24). Release: the two tripwire gtests use the same digest; the Release identity
  path note below.

## Decisions

- S2-3 "own cache identity" read as: a scope of its own, not the flag-absent scope (whose key the
  legacy IC would otherwise share): a `--label-terminal` run never writes into the scope flag-absent
  runs read, at the cost of one cold missing-exit-v1 pass per role the first time.
- `--no-composition` under `--label-terminal` skips loading a signal only when BOTH IC results hit
  (re-enabled; the WIP had disabled the shortcut).
- Kept from the handoff: table `OpTraits` / `kOpTraits` with `ChunkAxis` moved there; streaming
  `is_ts_op` / `is_cs_op` keep W2 rows out (exactly the old sets); bands = min(n, 4 x workers),
  Dates / Cells on the Cs pool, Instruments on the Ts pool, shared growth (state_, LoadField check)
  on the calling thread; imputed terminal = first session in (entry, end] the label role presents and
  the scored role does not; `--ic-hac-rule` given at its default is still recorded.
- 100 columns: the two S2 lines in `vm.hpp` wrapped; the impl IC files and `ic_screen.cpp` keep their
  owning files' dense idiom (they carried 119 / 76 / 25 / 14 lines over 100 at base).

## Deviations

- No separate `vm_band_parity_test.cpp`: worker parity runs over the slot-reuse fixture's every
  catalogue variant (every banded kernel included), both modes, with and without the mask. The 12 x 7
  panel splits each axis down to single dates / instruments at 4 and 16 workers and into multi-row
  bands at 1 worker; a wider-panel parity on real-size panels is covered by root's X-5 identity.

## Concerns / open risks

1. Root-only checks outstanding: X-5 u / w identity and the cold-u wall measurement.
2. The return guard is computed on the scored role, so an imputed terminal return is never
   guard-checked (a large delisting return is legitimate); flagging in case P9-L wants a rule.
3. `--eval-mode audit-exact` + Release: the audit root adds 6 characters (`_audit`) to the Release
   identity directory, which S1's carried minor already puts near the 259-character path limit.
4. `eval-mode-audit-exact` keeps its capability name while its meaning widens (now allowed with the
   cache); E2 reads capabilities, no consumer relied on the refusal (grep).
5. Not compiled-tested beyond equity-dev Debug; Release builds are root's.

## Carried minors

Carried S1 minors not fixed: each lives in S1's marginal-verb files (`strategy_marginal_ic.*`,
pair cache, `--verified-digests`, `--exclude-self`, test-only `cache_identity`), `CMakePresets.json`
/ verify list, or the unowned `research_ic_fitness.cpp` comment; all outside S2's files (forbidden or
unowned).

## Hygiene

Child processes: none left (checked). Scratch: lane scratch dir `s2-scratch/` (ignored) deleted
after this commit; the equity-dev object tree is deleted per DISK-2 keeping `mega-p9-s2-*` receipts
and logs. Pytest temp roots removed by the tests.

## Ledger candidates

1. C++20 range-for over `read_json(path).at(key)` dangles (the temporary JSON dies before the loop
   body; symptom "cannot use at() with number"): bind the parsed document to a local first.
2. A lane can check flag-absent byte identity without real data: record the tiny_world canary with
   BASE exes (`test_cycle_e2e.py --record`), check HEAD exes with `ATX_CANARY_REQUIRED=1`, restore the
   goldens file (S2: equal at 1239a5ff vs 4cb9265a, Debug).
3. P9 S2: source pins `dsl_vm` `5bcd55c7...` (35 files) and `ic_result` `bd13574c...` re-pinned
   without a semantics bump (S2-1).

## Fix round 1 (PM ruling S2-FIX1)

Code `110fd735` (fix) and `80d81b26` (tests), on the review commit `98c85cc9`. Review items (3) (to
the prereg) and (5) (noted for C3) are left as ruled.

1. **Return guard on the label role.** `ResearchIcTerminal` gains `label_bad_return_prefix`: the
   label panel's excluded-return prefix (`research_return_guard` run on the loaded label role, same
   shape as the scored one). Under `imputed-v1`, a straddling pair is guard-excluded when either the
   scored guard or the label guard flags a return in (entry, exit]. The terminal step T = L+1 is now
   checked: an absurd imputed return (|log r| > 1.5, or adjusted vs raw drift > .10) is excluded
   and counted in `guard_excluded_pairs`, never scored. The builder's Shumway set {0, -.30, -.55}
   passes (|log .45| = .80; the builder writes `raw[T]` and `close[T]` with the same factor).
   `prepare_research_ic` refuses an imputed-v1 terminal whose label guard is missing, has the wrong
   shape, or decreases. The runner frees the label guard after prepare. Concern 2 of the original
   report is resolved by this item.
2. **Terminations only.** `bind_label_role` computes each name's last scored-present session
   `last[i]` and refuses label-only presence anywhere except `d == last[i] + 1`:
   "IC runner: label role adds presence that is not a termination (date d, instrument i); a label
   role may only add the session after a name's last scored session". It also binds `raw_close`
   bitwise on scored-present cells, because the label guard reads `raw_close`. Under the flag only,
   the recipe `labels` string appends
   `;label-role-adds-only-the-session-after-each-name's-last-scored-session;imputed-exit-return-guard:scored-role-and-label-role(terminal-step)`,
   and the admission memory term grows by 4 B/cell for the label guard.
3. **Validation-only resume pin (item 4).** At `7ccf81e8` the empty TRAIN pin was not actually
   written. A validation-only run erases TRAIN from the scored `roles` before the pin loop, so the
   recipe already held `{"validation": <pin>}` only. This is now explicit: the pin loop refuses a
   scored role without a label-role pin (Internal: "scored <role> role without a label role pin").
   `IcLabelTerminal.FrozenTrainResumeBindsTheLabelRolePins` asserts the resumed recipe's
   `label_role_manifest_sha256` is exactly `{"validation": pin}`, and that the resumed `recipe.json`
   SHA equals the train-free run's.
4. **Tests (items 6 and 7).**
   - Engine `IcLabels.TerminalImputedPairsPassBothReturnGuards` (L=7, T=8). (A) A scored-role bad
     return at session 6, present in both guards: only entries >= 6 impute. (B) A bad terminal step
     in the label guard only: every h-straddling pair is guard-excluded. It also checks the
     refusals: a scored guard with no label guard, and a decreasing label guard.
   - Impl `IcLabelTerminal.AbsurdTerminalReturnIsGuardedOnTheLabelRole` patches the label role's
     `close` and `raw_close` at (420, 0) to 5% of (419, 0). It expects `terminal_imputed_pairs` 0,
     `guard_excluded_pairs` up by {5, 21, 33}, and IC bitwise equal to `missing_exit_v1`.
   - `IcLabelTerminal.LabelRoleRefusals` adds a doubled label role (presence at T+1), which is
     refused at "(date 421, instrument 0)".
   - The audit test plants the ResearchFast sidecar under the `_audit` root and expects
     "candidate cache entry mismatch: volume_level". It then restores the sidecar and the rerun
     is OK.
   - `IcLabelTerminal.MissingExitIcIsCachedUnderItsOwnScope` plants the default-scope IC record
     into the `missing_exit_v1` scope and expects "candidate IC cache entry mismatch:
     volume_level". It then restores the record, and the served run hits 2.
5. **Long lines (item 8).** The six engine-test lines are wrapped. The engine test now adds no
   line over 100 columns versus base `1239a5ff`. Five new dense-idiom lines in `ic_screen.cpp`,
   which the review did not list, are left alone: `ic_screen.cpp` is in `ic_result_sources`, so
   wrapping them would move the pin again.

**Digests.** `ic_result_sources` was re-pinned again under S2-1:
`bd13574c...` -> `dd3c601163029c1dc31a80ebbe155437a0d688a6731c84658637aac2913a94c2` (11 files;
`ic_research.hpp` and `ic_screen.cpp` changed). `dsl_vm_sources` is unchanged at `5bcd55c7...` (35
files). Both were recomputed at `80d81b26` with the recipe and equal the pins. No expected output
hash was edited.

**Builds.** These went through `scripts/research-build.ps1`, equity-dev, with the memory gate.
- `p9-s2-fix1-a`: source `98c85cc9` plus the 7 working-tree entries, exit 0, 365 TUs, 458 s.
- `p9-s2-fix1-b`: source `80d81b26`, DirtyEntries 0, exit 0, 7 TUs, 27 s.

Receipts and logs are `build-equity/mega-p9-s2-fix1-{a,b}-*`. The only warnings are third-party:
`/MP`, and databento `getenv`. The tree was configured at `98c85cc9-dirty`, so that is the stamped
provenance; the fix-b receipt pins the exact source.

**Tests at `80d81b26`.**
- `atx-impl-strategy-ic-tests`: 204/204.
- `atx-engine-alpha-tests`: 782/782.
- `atx-impl-strategy-mine-tests`: 44/44. `SignalFitnessDefaults` holds golden `0x889874a3b9b29c55`
  at 1 and 4 workers.
- pytest `atx-impl/tools/test_fit_composition_rule_plugins.py`: 8 passed.

**Flag-absent identity.** The tiny_world canary was re-run with the `80d81b26` Debug exes against
the base `1239a5ff` Debug record, with `ATX_CANARY_REQUIRED=1`. Result: 15 passed. The goldens file
was restored byte-exact and the tree is clean. The log is
`s2-evidence/canary-head-80d81b26-debug-check.log`.

**Hygiene.** No child processes remain. `s2-scratch/` is deleted, and the object tree is deleted
again per DISK-2, keeping receipts and logs.
