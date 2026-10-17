# Lane S2 review

## Verdict

**APPROVE** (0 Required, 8 Suggested).

- Spec verdict: **PASS.** Every brief deliverable and every carry and ruling item for S2 is implemented and tested:
  P16, P13 (IC half), S1-PIN, S2-1, S2-2 and S2-3. The carried S1 minors are correctly left alone, because each one
  lives in S1's marginal-verb files (forbidden to S2) or in unowned files.
- Quality verdict: **PASS.** I found no UB, lifetime, bounds or determinism defect. Flag-absent paths are
  value-preserving by construction. The two re-pins are recomputed source digests, and I reproduced them
  independently. The closed-form label test has teeth.

## Reviewed SHA

`7ccf81e8` (code head `4cb9265a`; base `1239a5ff`), branch `feat/p9-s2-20261003`, pool-18.

## Evidence

1. pytest, explicit path, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, inside pool-18:
   `python -m pytest -q -p no:cacheprovider atx-impl/tools/test_fit_composition_rule_plugins.py` -> `8 passed in 0.69s`,
   exit 0. This suite reads the S2-2-regenerated `composition_rules_list.json`.
   `python -m pytest -q -p no:cacheprovider --basetemp=<scratch> scripts/tests/test_cycle_e2e.py` -> `14 passed, 1 skipped
   in 3.24s`, exit 0. The canary is skipped because the build tree is gone. Its basetemp was deleted.
2. Source-pin re-pins (S2-1), recomputed independently from git blobs. A scratch script parses each `*_sources` list
   from the owning .cpp and applies the tripwire's recipe: per path in list order, `path\n<len>\n<text>`, CRLF->LF,
   then SHA-256.
   ```
   1239a5ff dsl_vm_sources    34 pin fcf8021e76ee5ac3 recomputed fcf8021e76ee5ac3 EQUAL
   1239a5ff ic_result_sources 11 pin 2d758bff0cfb5090 recomputed 2d758bff0cfb5090 EQUAL
   7ccf81e8 dsl_vm_sources    35 pin 5bcd55c788144513 recomputed 5bcd55c788144513 EQUAL
   7ccf81e8 ic_result_sources 11 pin bd13574c08a61e4c recomputed bd13574c08a61e4c EQUAL
   ```
   Both old pins reproduce from the base blobs, and both new pins are the digest of the committed head blobs. They are
   source-file digests, not expected outputs. The file set is the constexpr list in its declared order, and
   `op_traits.hpp` joins the VM list. The tripwire's closure check (every included `atx/engine` header must be
   listed) still applies. No expected output hash was edited.
3. Exact-SHA build and gtests. I re-ran these because the lane's receipts show that its test binaries were never
   built at a committed SHA:
   - `atx-engine-alpha-tests` was last built in tag `c` (`eff81c86`-dirty, exit 1).
   - `atx-impl-strategy-ic-tests` was last built in tag `e` (`eff81c86`-dirty).
   - Tag `p9-s2-head` (`4cb9265a`) built only the ic, targets and mine-tests targets.

   The report's "head sources" wording therefore went beyond its receipts. I configured `equity-dev`, then ran
   `scripts/research-build.ps1 -Tag p9-s2-rv-a -Targets atx-engine-alpha-tests,atx-impl-strategy-ic-tests`.
   - Receipt `build-equity/mega-p9-s2-rv-a-receipt.json`: Source `7ccf81e8`, DirtyEntries 0, ExitCode 0, 361 TUs.
     The log has 0 first-party warnings (two third-party databento `getenv` deprecations only).
   - `atx-engine-alpha-tests.exe --gtest_filter=OpSig.*:AlphaVmSlotReuse.*:AlphaVmBandParity.*:AlphaVmTruncation.*`
     -> `[  PASSED  ] 17 tests.`, exit 0. The whole group gave `782 tests from 314 test suites ran ... [  PASSED  ]
     782 tests.`, exit 0.
   - `atx-impl-strategy-ic-tests.exe` (whole) -> `202 tests from 30 test suites ran ... [  PASSED  ] 202 tests.`,
     exit 0. This includes `IcLabels.TerminalImputedClosedForm`, `IcScreen.MinCoverageConfig`,
     `IcScreen.HacRuleNamedSelectableAndReproduced`, `IcLabelTerminal.*` (4), `IcMethodOptions.*`,
     `IcFieldsSeal.*`, `StrategyIcRunner.AuditExactEvalModeIsRecordedAndCachedUnderItsOwnIdentity`, both
     `*SourcesPinnedToSemanticsVersion` tripwires and `CompositionRules.ListRulesJsonPinned`.
   - The object tree I configured was then deleted by name. The `mega-p9-s2-rv-a-*` receipt and logs, the lane's
     receipts, and the pre-existing `audits/` and `v8-interim3-pitch-render-run2/` are kept. My test processes have
     exited, so no child process is left.

## Findings

`path:line | severity | problem | fix`. Required = must fix before merge; Suggested = should fix, root may defer.
None is Required.

1. `atx-engine/src/factory/ic_screen.cpp:400` (with `atx-impl/src/strategy_ic_runner.cpp:368`) | **Suggested** |
   The lane's concern 2 is correct: the return guard is the scored role's prefix (`research_return_guard(role)`), so
   the imputed step `close_label[T]/close[L]` is never checked. I judge this **not a live defect**. The only label
   source the runner admits is the role builder's `--delisting-returns` build. Its rule
   (`prepare_recent_research.py` `DELISTING_RETURN_RULE`) sets `close[T]=close[L](1+r)` and
   `raw_close[T]=raw_close[L](1+r)`, with `r` in the Shumway set {0, -.30, -.55}. Recomputed on that step, the guard
   would never fire:
   - `|log(1+r)| <= 0.80 < 1.5`;
   - the adjusted-vs-raw test is equal by construction.

   Applying the price guard to a delisting return would also re-create the survival bias the label is meant to
   remove. An `r <= -1` already counts as `invalid_price_pairs` (`to <= 0`). The gap is that the C++ side trusts any
   manifest declaring `returns_applied: true`: nothing binds the builder's `return_rule`, and nothing bounds `r`. A
   later builder rule using raw DLRET (unbounded, data errors included) would therefore reach Pearson IC unchecked.
   Fix (one of):
   - in `admit_label_role`, require the label manifest's `universe.delisting.return_rule` to start with
     `delisting-return-on-termination-v1`;
   - compute the guard for the terminal step on the label role (its close and raw_close at L and T);
   - record the decision in the recipe's `labels` text.
2. `atx-impl/src/strategy_ic_admission.cpp:410-429` (`bind_label_role`) | **Suggested** | The refusal text says
   "a label role may only add presence (its termination sessions)", but the check only requires equality on
   scored-present cells. Any extra presence is accepted. `terminal_session` (`ic_screen.cpp:307`) then takes the
   **first** label-only session in `(entry, end]` as an exit. A label role that adds presence at an interior gap
   would therefore mis-impute a non-terminal exit. The builder never does this (it skips "present after last
   session"), so this is hardening only. Fix: also refuse any label-only cell `(s, i)` where the scored role presents
   `i` at some session after `s`. That is O(cells), one reverse scan per column. Add a refusal case to
   `IcLabelTerminal.LabelRoleRefusals`.
3. `atx-impl/src/strategy_ic_admission.cpp:58` (recipe `labels`) and `ic_screen.cpp:303-312` | **Suggested** |
   Look-ahead of the imputed value is within the label's licence (`T <= end`, label rows mature inside the role, and
   nothing imputed reaches a signal; see Checked). One caveat goes unrecorded. The builder's rule says the
   termination's cause, which selects `r`, "may be classified after T" (`available_at` up to 30 days later). A TRAIN
   label whose `T` falls within about 30 days of TRAIN's maturity end can therefore embed a classification dated in
   the validation period, and the frozen TRAIN orientation is fitted on it. The size is tiny, and the role builder
   already drops rows `available_at >= seal`, so the seal holds. Fix: state the clock in the recipe `labels` text and
   the lane report, and flag it for the P9-L adoption ruling (purge the imputed label when `available_at` > the
   role's maturity end, or accept it explicitly).
4. `atx-impl/src/strategy_ic_runner.cpp:872-874` | **Suggested** | The comment says "each scored role's label role
   pin", but the loop runs over `roles`, which includes the **unscored** frozen TRAIN in validation-only mode. That
   has two effects:
   - a resume that omits the optional `--train-label-role-sha256` writes `"train": ""` into `recipe.json`;
   - two resumes of identical computation (with and without the optional train pin) get different `recipe_sha256`.

   TRAIN's real pin already travels in `frozen_train_recipe`. Fix: under `validation_only`, skip TRAIN, or write
   `recovered`'s pin. Assert the recipe bytes of the `resumed` and `train_free` runs in
   `IcLabelTerminal.FrozenTrainResumeBindsTheLabelRolePins`.
5. `atx-engine/include/atx/engine/alpha/vm.hpp:829-836` (`BandScratch::asof`) vs
   `atx-impl/src/strategy_ic_admission.cpp:330-339` (worker term) | **Suggested** | Banding replaces the one
   `asof_scratch_` with one `AsofScratch` per worker. Its `rank` buffer holds `d x n` f64, so as-of memory now scales
   with workers: at `d = 126`, `n = 5,000` that is about 5 MB per worker, or 80 MB at 16. The admission's per-worker
   term (`1024+64+16` B/name, 64 B/date, 8 MiB stack) was sized before any per-worker as-of scratch existed. It
   covers the per-worker `rank` buffer only while `d <~ 128`. Fix: add `workers x n x 8 x max_asof_window` to the
   worker term when the library uses an as-of op (or show the bound in the report). Root's cold-u measurement at the
   P9 worker count is the practical check.
6. `atx-engine/tests/factory/ic_screen_test.cpp:907-1004` (`IcLabels.TerminalImputedClosedForm`) | **Suggested** |
   The test never passes a return guard (`bad` is empty), so the ImputedV1 guard path is unexercised:
   `bad[exit*n+i]` with `exit = T` counts scored-role bad returns over `(entry, L]`. Fix: add one guarded
   variant. Plant a bad return on name 0 between `entry` and `L`, and expect the straddling pair counted in
   `guard_excluded_pairs`, not imputed.
7. `atx-impl/tests/strategy_ic_runner_test.cpp:3016`, `:4623` | **Suggested** | The new cache scopes are separated by
   path and key, and the code refuses a foreign record: `v2_payload_sha` checks `vm_identity`, `eval_mode` and
   `signal_key`, and `ic_cache_lookup` compares the full `key`. No test plants one, though. Fix: in the audit test,
   copy a ResearchFast sidecar into `<identity>_audit/<role>/` and expect "candidate cache entry mismatch". In the
   S2-3 test, copy a flag-absent IC record into the `missing_exit_v1` subdirectory and expect "candidate IC cache
   entry mismatch".
8. `atx-engine/tests/factory/ic_screen_test.cpp` (6 new lines), `atx-impl/src/strategy_ic_runner.cpp` (22),
   `strategy_ic_admission.cpp` (15), `atx-impl/tests/strategy_ic_runner_test.cpp` (59) | **Suggested (style)** |
   These added lines exceed 100 columns. For the impl IC files this follows their dense idiom, which already had many
   long lines at base, as the report says. The six in the engine test are new code in a mostly wrapped file. Fix:
   wrap those six; the rest are root's call.

Not findings (checked and accepted): see Evidence 3 for test-binary provenance. The brief's "OpSig table" became a per-opcode
`OpTraits` table (`kOpTraits`): `OpSig` rows are per DSL name, and several names share one opcode, so a per-opcode
table is the right shape. `lookahead_safe` is derived from it, although today every family returns true, so the
derivation is a placeholder until a forward-reading family exists.
`eval-mode-audit-exact` keeps its capability name while its meaning widens; no consumer relied on the refusal.

## Checked

**Label-terminal (`--label-terminal imputed-v1`).**
- Look-ahead and leakage. `terminal_session` scans `(entry, end]` only. The exit price is
  `label_prices[T]`, with `T <= end <= maturity_end`. The decision and entry prices, the decision membership and the
  guard all come from the scored role. The label role is read through `read_strategy_role` and
  `bind_label_role` (axes, session keys, ids, score window, and bit-equal close and presence on every scored-present
  cell). It is passed only to `prepare_research_ic` and dropped (`label_role.reset()`, runner:396) before
  `prepare_research_ic_scratch` and any VM. Nothing imputed reaches a signal, field, composition input, guard or
  membership. The orientation sign is learned from the imputed IC, which is the label path by design and flag-present
  only.
- E-10 stays in force for every scored role; only the pinned label role is exempt.
- The `bbc7da8d` fix is right. Under the old `to = exit == end ? prices[end] : label[exit]`, any pair whose
  termination session **is** the horizon end read the scored role's NaN close and was counted invalid. That is the
  earliest straddling entry, `entry = L+1-h`, in **every** horizon, not only h = 1. The explicit `imputed` flag
  selects the label panel.
- The closed-form test has teeth. Its straddling set is `entry in [L-h+1, L]`, which includes `entry = L+1-h`, for
  `h = 1..4`. Each straddling cell must equal `terminal_close/close[entry]-1` bit for bit, and the test also asserts
  `imputed_cells == h`, `terminal_imputed_pairs == h` and `invalid_price_pairs == 0`. The old code fails all three in
  every horizon. The expected value is built from the fixture constants, not from the code under test.
- Every non-straddling label is bit-equal to MissingExitV1. MissingExitV1 named with a label panel ignores it. The
  refusals cover a null panel, other axes, a renamed field and an unknown rule.

**Cache identity.**
- Signal cache. `vm_identity(true)` appends `_audit`, so the root is `DIR/<identity>_audit`. It never equals the
  legacy identity (so the v1 `legacy_entry` path is closed to audit runs) and never equals another flavour's root:
  `build_flavor_suffix` ends in the xsimd version and cannot end in `_audit`.
- `CacheKey::eval_mode` enters `signal_key_text`, the sidecar and `same_geometry`. A stale or foreign sidecar under
  the audit root fails `v2_payload_sha` (vm_identity, eval_mode and signal_key) as a loud "entry mismatch".
- IC-result scope. The `ic_cache_scope` key gains `min_coverage_bits` / `hac_rule` only when non-default.
  `--label-terminal` always adds `label_terminal`: `imputed-v1` together with `label_role_manifest_sha256`, or
  `missing-exit-v1`. A flag-absent key never carries `label_terminal`, so neither label-terminal scope can equal it.
  The 16-hex directory prefix is backed by a full-key comparison in `ic_cache_lookup`, so a prefix collision refuses
  rather than serving.
- The only shared scope is `--ic-hac-rule bartlett-2h-v1` or `--min-coverage 0.8` given at its default. That is
  semantically identical, the stored record carries no print-only `hac` block, and the test asserts the hit.
- Audit IC-result entries live under the audit signal directory and are keyed on the signal payload SHA-256.
- I found no collision.

**Flag-absent byte identity.**
- `prepare_research_ic(..., terminal={})` defaults to MissingExitV1. That gives empty `label_prices`, so a missing
  exit still means `continue`, in the same counter order, with `exit = end` and `to = prices[end]`.
- The Bartlett estimate is the old code moved into the `else` branch, with `hac_lag` computed identically.
- `cfg.min_coverage` (0.8) replaces `kMinCoverage` (0.8) as the same double in the same expressions.
- Recipe keys, summary keys, the result-record `terminal_imputed_pairs`, the `hac` / `missing_exit_share` print and
  the scope keys are all conditional.
- `Json row{...}` builds the same object that `push_back({...})` did.
- `signal_key_text` and the sidecar `eval_mode` resolve to `vm_eval_mode`.
- `Library()` derives `lookahead_safe = true` for every row, as the literals said. `register_op` keeps the declared
  value for every in-enum opcode.
- No iteration order changed: nlohmann objects are sorted, and the per-candidate loop is unchanged.
- The new progress line `IC fields-seal ... accepted-as-legacy-manifest` appears only for a seal-less manifest,
  which P13 requires to be logged.

**What the canary (5 digests, tiny_world, Debug) does not cover:**
- Ops. Its four members use only LoadField, Const, Mul, Div, `decay_linear` (the existing pooled Ts kernel) and
  `rank`. None of the newly banded kernels (W2 Ts/Cs, the as-of family, recurrences, Split2, KalmanReg, ArgPack,
  GroupCross) or the `eval_lit_ts` column-extraction rewrite is reached.
- Data shape. Every cell is finite, with no universe holes.
- Build type. Only Debug equity-dev is covered.
- Caches. It runs cold, so it says nothing about pre-S2 signal and IC-result entries still hitting under the S2 exe
  (the "no cold pass" claim).
- Outputs. It does not cover `summary.json`, `recipe.json`, the daily IC CSVs, the candidate ledger or planned
  targets, except as they feed the u/fit/w/NAV digests.
- Run modes. It does not cover validation-only frozen TRAIN, `--no-composition`, the marginal or mine verbs, a
  real-size panel at 4 or 16 workers, or a seal-less fields manifest (tiny_world carries a matching seal block).

These are root's X-5 u/w identity run, the golden at 1 and 4 workers, and the Release build.

**HAC rule and min-coverage.**
- Bartlett2hV1 (default) is `mean_inference(work, Bartlett, max(2h, NW-rot), true)` with the IID floor,
  unchanged.
- HorizonAwareV3 is `hac::mean_tstat(work, HorizonAwareV3, h)`: uniform kernel at `max(h-1, rot)`, cancellation
  guard and Bartlett fallback for h > 1; Bartlett at the NW auto lag for h = 1. It has no IID floor and the same
  `T/n` scaling, with the lag actually used recorded. This matches the brief and DSL/IC review §3 (comparable with
  the combine path).
- min_coverage is validated in (0, 1] (NaN and inf refused) in both the engine (`validate`) and the runner.
  Boundary: every gate refuses with strict `<`, so equality passes, as before. Empty series and zero valid dates
  return early, undefined. Empty segments fail the count floor.
- `ic_screen_cache_matches` binds both new fields.

**Worker parity.**
- `run_bands` splits `min(n, 4 x workers)` contiguous non-empty bands that exactly partition `[lo, hi)`. Each band
  writes only its own rows, columns or cells with the unchanged serial kernel and its own `BandScratch`.
- Shared growth (`state_` for TradeWhen/Hump, the LoadField remap check) runs on the calling thread before any band.
  `band_thr_` is resized only by the pool setters.
- There is no new nesting hazard. The IC runner drives evaluate from the main thread, and `global_dag_eval`'s
  engines set no pool.
- Element-wise kernels are scalar per cell (no libm SIMD), so a band boundary cannot change a bit.
- `AlphaVmBandParity.*` runs **every** fixture variant (all catalogue rows, now including `formulaic_ops()`) at 1, 4
  and 16 workers. The Catalogue test asserts every non-housekeeping opcode is reached (`seen[v]`, `kOpCodeValues =
  256 >= kOpTraits.size()`).
- The 12x7 panel splits each axis down to single dates, columns and cells at 4 and 16 workers, and into multi-row
  bands at 1 worker.
- The golden at 1 and 4 workers ran green at the head build (`p9-s2-head`, which includes the mine tests).

**OpTraits.**
- `op_traits_row` is an exhaustive switch with no default.
- `static_assert(kOpTraits.size() == AsofRankCov+1)` holds, and the consistency `static_assert` covers stateful
  (iff recurrence), group (CrossSection only) and axis per family.
- `OpSig.PredicatesAgree` compares every derived predicate of typecheck, VM, streaming and oracle against the base
  hand lists, and I checked the lists against the removed code: cross-section 19, grouped 9, shift 3, rolling 48,
  streaming Ts 39, streaming Cs 17, and the VM instrument rule as its numeric range.
- The remaining `case OpCode::Cs*` lists are kernel-dispatch switches, which is allowed.

**Cross-lane edits (S2-2) and scope.**
- `exe_capabilities` gains three entries inserted in sorted order. The K-P9-6 contract requires sorted, so this is
  additive rather than literally appended; no entry changed. The fixture gains the same three lines, and
  `ListRulesJsonPinned` checks one list.
- `ic_research.hpp` is justified. It declares the research-IC API (`prepare_research_ic`, `ResearchIcCoverage`)
  whose definitions live in S2's `ic_screen.cpp`, and it is already in `ic_result_sources` (covered by the S2-1
  re-pin).
- There is no CMake edit (the alpha tests glob), no marginal-verb or composition edit, and no new schema literal.

**Hygiene and policy.**
- Build log for `p9-s2-head` (`4cb9265a`, ic, targets and mine-tests): 0 `warning:` lines, exit 0, 106 TUs.
- No trial run and no ledger or `LEDGER.md` append are in the diff. The evidence files are synthetic tiny_world
  digests. Nothing dated 2024-01-01 or later was opened.

- [x] `.agents/cpp/agent.md` §10 checklist applied to the diff
- [x] Diff stays inside the brief's files in scope plus P16, S1-PIN, S2-2 and the justified `ic_research.hpp`
- [x] Evidence in the report matches its claims (re-pins recomputed, pytest re-run, gtests re-run at the exact SHA)

## What root must check at merge

1. The X-5 u/w flag-absent identity, the cold-u wall before and after, and the golden at 1 and 4 workers, per the
   brief. Root also measures the Release + audit-exact path length (S2-4).
2. Textual conflicts expected against D2, AL-COMB and SQL1. All three touch `strategy_ic_rules.cpp`,
   `strategy_ic_runner.cpp` and `composition_rules_list.json`. SQL1 also touches `strategy_ic_admission.cpp` and
   `strategy_ic_detail.hpp`, and D2 and SQL1 touch `strategy_ic_runner_test.cpp`. Keep `exe_capabilities` sorted and
   its array size equal to its entry count.
3. P13 is now enforced in C++ for every flag-absent run: a fields manifest whose `seal.exclusive_end` is present and
   not `2024-01-01` is refused, in the IC runner and in `strategy_research_role.cpp`, which shares `bind_fields`.
   Confirm every live X-5 / Y fields manifest has either no seal or that exact seal before the first P9 cell.
4. Release builds of the lane targets: both the lane and this review built Debug `equity-dev` only. The Debug test
   groups are now green at the exact SHA (Evidence 3). Still open: Release, `atx-engine-factory-tests` (where
   `ic_screen_test.cpp` joins a unity batch) and the `hygiene` include preset.
