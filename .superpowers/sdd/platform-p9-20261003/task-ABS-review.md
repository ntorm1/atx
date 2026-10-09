# Lane ABS review (first review; P9 RESUME 9)

## Verdict

**APPROVE.** Required 0, Suggested 6.

## Reviewed SHA

- HEAD `5dae4f69` on `feat/p9-abs-20261006`, pool-28. The tree was clean before and after the review.
- BASE (REV-BASE) `fcb0200e` = `git merge-base 5dae4f69 feat/platform-p9-20261003`.
- D3W's APPROVE head `f0726acb` is not in root yet, so it is excluded too.
- Lane-own commits: `git log --no-merges 5dae4f69 ^f0726acb ^fcb0200e`. They are `55464a2e`, `1698209b`, `f92c5737`,
  `40b8282e`, `bdafd1a9`, `5f416d32`, `7a748630`, `265bb427`, and the records commit `5dae4f69`.
- Code diff: `git diff f0726acb 5dae4f69 -- atx-impl/src atx-impl/tests atx-impl/CMakeLists.txt atx-engine`.
- Root's share of that range (`fd084e78..fcb0200e`) touches no IC runner or composition file. Its C++ is only
  `eval_report.{hpp,cpp}`, `atx-engine/tests/CMakeLists.txt` and two eval tests. The lane's code files are
  `atx-impl/CMakeLists.txt`, `atx-impl/src/strategy_ic_{absent.cpp,absent.hpp,composition.cpp,detail.hpp,rules.cpp,
  runner.cpp,runner.hpp}`, `atx-impl/tests/{fixtures/composition_rules_list.json,strategy_ic_rules_test.cpp,
  strategy_ic_runner_test.cpp}`.
- All three merges are plain: `git merge-tree --write-tree` of each one's parents gives exactly its tree. They are
  `58617960` (D3W `f0726acb`), `4144915a` (root `685e238b`) and `60094dcc` (root `fcb0200e`).

## Spec verdict (brief-ABS, contract `od3p-absent-contract.md` sections 2-5, rulings ABS-1 .. ABS-5, OD3P-1, D3W-S)

| item | verdict | where / evidence |
|---|---|---|
| First step: merge D3W `f0726acb`, then root; never rebase | DONE | Merges above, all plain. |
| Flags `--absent-members` / `--absent-members-sha256`, both or neither, each at most once | DONE | Pair check: `strategy_ic_absent.cpp:61-62`. Repeat check: `strategy_ic_runner.cpp:1142-1148`, exit 2. |
| Every refusal of contract section 5 comes before any output, and each has a test with teeth | DONE | See the table below. Checked against the case list in `strategy_ic_runner_test.cpp:5497-5604` (cases `:5523-5566`). The lane's `p9-abs-c` log shows each of the 27 cases with its own 5 failure lines, plus the 4 CLI cases. |
| Semantics: a listed member is a member with no data on every date | DONE (ABS-4 route) | `strategy_ic_runner.cpp:533` sets every cell of the signal to NaN. This happens after the candidate cache has served or stored the real bytes, and before IC, orientation and `IcComposition::add`. |
| Masses fixed at create; no-theme blend keeps its fixed denominator | DONE | The composition is built before the candidate loop (`:448-450`) from the pinned or effective weights, so no signal reaches it. An all-NaN signal adds nothing: `strategy_ic_composition.cpp:365-376` gives `row.size() < 2` on every date. Cases 1, 2 and 5 show this byte for byte. |
| v3 walk-forward rows | DONE | Case 7, `WalkForwardRowsTreatTheMemberAsMissing`, on the plain path and on ew-theme-std-v1. |
| `summary.json` `absent_members {rule, list_sha256, members}`, only with the flag | DONE | Record: `strategy_ic_runner.cpp:945`, `strategy_ic_absent.cpp:78-82`. Test: `RecordIsTheOnlyTraceAndUnlistedRowsAreUnchanged` (key set equal apart from the record; `recipe.json` same bytes). |
| Cache statement | DONE | Report, "Cache statement". Code: `:549-550`, `:504`. Test: `SharedCacheNeverCrossServes...`. My own teeth below. |
| `--no-composition` with the flag is refused | DONE | `strategy_ic_absent.cpp:64-65`; case `screen`. |
| Capability `absent-members-v1` in `--list-rules --json`, sorted | DONE | `strategy_ic_rules.cpp:553-556` (first in a 9-entry array, ASCII-sorted). Tests: `CapabilityIsListedInTheSortedEnvelope`, `CompositionRules.ListRulesJsonPinned` (`strategy_ic_rules_test.cpp:412-413`). |
| Equivalence cases 1-6 (+ v3 rows), byte for byte over every published file | DONE | `expect_absent_equivalence` (`strategy_ic_runner_test.cpp:5254`): same file set with at least 10 files; binary files byte for byte; text after the ABS-2 substitution; `summary.json` and `*.jsonl` without the contract's keys. A third run (the data, no list) must differ, so no case is vacuous. Cases 1-7 at `:5290-5375`. Per-member rows: proved for every file (contract section 5's fallback is not needed). |
| Flag absent = bytes unchanged | DONE | By construction: an empty mask makes every changed condition reduce to the old one (`:504`, `:549-550`, `:551`, `:568`, `:577`, `:588`). There is no record, recipe term or cache key without the flag. The S-3 helper is the old lambda exactly. No existing golden or expected value was edited; no existing assertion changed (only S-7 reflow and one capability added to a list). The named identity filter passed 53 / 53 (lane's `p9-abs-b`). Whole target and canary are deferred to the gate (TEST-1). |
| D3W-S S-3 (REQUIRED in ABS): one private `weight_at` | DONE | Declared `strategy_ic_composition.cpp:210`, defined `:215-219`, used at `:365` (add) and `:423` (add_standardised). No lambda is left. Semantics are the same as before. |
| D3W-S S-7: reflow the 19 lines | DONE | 561 -> 542 lines over 100 columns in `strategy_ic_runner_test.cpp`. I checked each reflow hunk with a whitespace-stripped comparison: all whitespace-only, except two comment touches (a split `// value.`, and one comment reworded with a capital and a semicolon). The lane adds 0 lines over 100 columns anywhere. |
| Builds / suites | DONE for what TEST-1 leaves | Debug `p9-abs-a`/`-b`, teeth `p9-abs-c`. Whole `atx-impl-strategy-ic-tests`, Release and the canary are deferred to the gate (TEST-1). |
| Report / handoff | DONE (see S-1, S-5) | Covers each refusal (file:line, test, teeth), cases 1-7, the identity procedure, what root re-proves, cross-lane edits and open questions. Every file:line anchor I checked is exact at HEAD. |
| Out of scope (Python side, NAV exe, kind files, part B) | untouched | No file outside `atx-impl` IC runner / composition / rules scope (plus the CMake tail and one detail-header comment line) is in the lane's commits. |
| `scripts/tests/test_research_cycle_od3.py` | not applicable | Not in pool-28's tree (OD3P is not merged), and it reads fake exes only (`:222`, `:345`, `:358` at pool-21). Not run. |

### Refusals (contract section 5) -> code -> test -> teeth

| refusal | code (HEAD) | cases | teeth |
|---|---|---|---|
| one flag without the other | `strategy_ic_absent.cpp:61-62` | `path_only`, `pin_only` | lane R1 |
| repeated flag (exit 2) | `strategy_ic_runner.cpp:1145-1146` | CLI, each flag x run / plan | lane R14 |
| with `--no-composition` | `strategy_ic_absent.cpp:64-65` | `screen` | lane R2 |
| pin not 64 lowercase hex | `:66-67` | `upper_pin`, `short_pin` | lane R3 |
| file missing / unreadable / empty | `:68` (`metadata_text`, binary read) | `missing`, `empty` | lane R4 |
| SHA-256 mismatch (bytes as read) | `:69-71` | `mismatch` | lane R5 |
| not JSON / not an object | `:72-73`, `:30` | `torn`, `array`, `scalar` | lane R6, R7 |
| `schema` missing / other / not a string | `:31-34` | `no_schema`, `schema_v2`, `schema_number` | lane R8 |
| `members` missing / not an array / empty | `:35-37` | `no_members`, `members_text`, `members_object`, `members_empty` | lane R9 |
| non-string member | `:43` | `number`, `null`, `nested` | lane R10 |
| id off `[A-Za-z0-9_]+` | `:45` | `dash`, `blank`, `space` | lane R11 |
| id not a candidate of `--library` | `:46-48` | `case`, `other` | lane R12 |
| id listed twice | `:49-50` | `twice` | lane R13 |

Each case runs as a run and as a plan, with both roles' `close.f64` removed. `expect_early_refusal` checks four
things: the run is refused, the message matches, the log is empty, and no output dir exists. The list check is at
`strategy_ic_runner.cpp:803`. That is after library, themes and weights, and before `cache_preflight`, admission,
any payload and `create_directory`.

## Ruling checks

**ABS-2 (equivalence with I'''s input pins substituted): PASS.**

The excluded set is exactly what the test code applies:

1. `drop_excluded` drops these keys at any depth, in `summary.json` and `*.jsonl` only (`:5198-5210`):
   `wall_seconds`, `stage_seconds`, `hash_seconds`, `candidate_cache`, `signal_cache`, `ic_result_cache`,
   `ic_missing_exit_v1_result_cache`, `absent_members`.
   - With `warm` it also drops `verify_bytes`, `field_loads`, `peak_resident_fields` and `loaded_bytes`. This is
     used only by the shared-cache test, never by cases 1-7.
2. `substituted` replaces values, not paths, in every text output of run 2 (`:5187-5194`; pins from `gate_pins` `:5168` and `run_hashes` `:5175`). It uses these values:
   - the gate fields manifest SHA-256 of each role (F_t, F_v);
   - the `gate.f64` payload SHA-256 of each role (G_t, G_v);
   - `recipe_sha256` (R), `orientation_recipe_sha256` (O), `orientations_artifact_sha256` (A);
   - each role's `combined_artifact.manifest_sha256` (M_r).

From the writers, those values land at these paths:

- F: `recipe.json` `research_fields.manifest_sha256.{train,validation}`; `summary.json` top-level
  `research_fields.manifest_sha256.*` and `roles[i].research_fields.manifest_sha256`; `orientations.json`
  `research_fields.manifest_sha256`; `<role>_combined.json` `research_fields_manifest_sha256`.
- G: `summary.json` `roles[i].research_fields.files.gate.sha256`. Also
  `roles[i].candidate_cache.entries[].field_payload_sha256`, which is already dropped as cache bookkeeping.
- R: `summary.json` `recipe_sha256`; `orientations.json` `recipe_sha256`; `<role>_combined.json` `run_recipe_sha256`.
- O: `summary.json` `orientation_recipe_sha256`.
- A: `summary.json` `orientations_artifact_sha256`; `validation_combined.json` `orientations_artifact_sha256`.
- M_r: `summary.json` `roles[r].combined_artifact.manifest_sha256`.

Why each one may be excluded:

- F and G are hashes of I'''s inputs, by construction.
- R, O, A and M_r are hashes over files that are themselves published and compared after the same substitution
  (`recipe.json`, `orientations.json`, `<role>_combined.json`). So a difference caused by the flag cannot hide
  behind one of them.
- Hashes not in the set are compared raw and are equal. These include `library_sha256`,
  `composition_weights_sha256`, `orientation_candidates_sha256` and every payload receipt.

Nothing else is excluded. The report lists the same set by identity rather than as JSON paths, and omits the
warm-only keys: S-5.

**ABS-3 (envelope fixture regenerated by the exe): PASS on substance. The ruling's wording needs a PM note.**

- (a) The fixture diff is exactly one added line, `"absent-members-v1",`, in the `capabilities` array
  (`--numstat` 1 / 0).
  - It is the new capability, not "a top-level key `absent_members`" as the ruling text says. The envelope has no
    such key; `absent_members` is a `summary.json` key, and no fixture holds it.
  - I read the ruling as "one line, naming the ABS addition", and that holds.
- (b) Nothing moved: 0 deletions. The fixture's nine `recipe_sha256` values and every numeric are unchanged.
- (c) Lane-only, `git diff --stat fcb0200e 5dae4f69` over `*fixture*` / `*golden*` / `*expected*` shows only
  `atx-impl/tests/fixtures/composition_rules_list.json`.
  - Over `f0726acb..5dae4f69` the other six fixture paths (eval_tie, research_eval_edges, `wave_fixture.py`) are
    root's: that diff, minus this file, is byte-identical (same sha256) to `git diff fd084e78 fcb0200e` over the
    same globs.
  - `ListRulesJsonPinned` reads the fixture at run time and passed in the lane's 53 / 53. The Python readers test
    membership only (`frozenset`, `phases.py:57`), never the exact list.

**ABS-4 (blanking in `score_role` instead of an `IcComposition` mask): PASS on (a)-(d); S-3's helper landed.**

- (a) **Cache isolation holds.**
  - Signal cache: `candidate_signal` stores the VM output synchronously in `cache_store` (`strategy_ic_runner.cpp:312-316`;
    `strategy_ic_signal_cache.cpp:469-501` copies from the buffer to the file and returns a `CacheHit` (path + sha),
    with no deferred writer). The NaN fill comes after it returns (`:533`). On a hit, `cache_load` reads into the
    `std::vector`, with no write-back. So no blanked byte ever reaches a signal entry, and the key is the unchanged
    real-signal key.
  - IC-result cache: for a listed member it is neither looked up nor stored, in both scopes (`:549-550`). The
    payload-not-loaded probe is skipped too (`:504`; also unreachable, since the flag is refused with
    `--no-composition`).
  - A later candidate never stores a stale blanked buffer. A miss releases the buffer and takes a fresh VM output;
    a hit overwrites it.
  - My own teeth (below) took away only the main-scope bypass: the shared-cache test went red.
  - Gaps that are only test gaps (the code is right): the signal-cache write direction (S-2) and the
    `--label-terminal` scope's bypass (S-3).
- (b) **Flag absent = bytes unchanged**, see the spec table.
- (c) **Per-member rows show no data.**
  - The listed member's IC rows, daily series and TRAIN orientation (sign 0) are I'''s, because the blank comes
    before IC scoring. Cases 1-7 compare `<role>_daily_ic.csv`, `<role>_candidates.jsonl`, `orientations.json` and
    the combined set.
  - `RecordIsTheOnly...` checks the listed rows end `,,,` on every date, and that the unlisted member's rows are
    unchanged.
  - The v3 rows are case 7.
- (d) **The report names the point** (items table `strategy_ic_runner.cpp:533`; Deviation 1) and says why no mask is
  needed: the composition input is byte-identical to I'''s. So every rule (redistribute, standardise, resid,
  schedules, sleeves, hedge, mom-volman, v3 rows) applies its own missing-member rule, and a mask would be a second
  mechanism. That matches D3W's "For ABS". It is one rule, reused.

**ABS-5:** the listed member is still evaluated (one VM pass per listed member). This is P10, as ruled.

## Teeth (TEST-1: ONE target-scoped Debug build, tag `p9-abs-d`; TEETH-RESTORE)

My own tool, kept outside the repo, applied one byte-exact mutation:

- in `atx-impl/src/strategy_ic_runner.cpp:549`, `const bool ic_cached=ic_scope.has_value() && !listed;` became
  `const bool ic_cached=ic_scope.has_value();`.
- That is the IC-result-cache bypass, main scope alone. The lane's S1 removed it together with the
  `--label-terminal` scope's.

Run:

- The tool waited for a quiet host first; another session's build was running. It kept the bytes, then built
  `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` through `scripts/research-build.ps1`.
- Receipt: Source `5dae4f69`, DirtyEntries 1, 340 TUs, exit 0. Teeth exes: ic `119de491...`, tests `f0502df5...`.
  They are never evidence and are deleted.
- It then ran `--gtest_filter=AbsentMembers.*`.

Result: **10 passed, 1 FAILED, as predicted.**

- `SharedCacheNeverCrossServesRunsWithAndWithoutTheList` failed at `:5406`: the warm listed run had
  `ic_results.hits` 3, not 2.
- It also failed at `:5409`: `volume_vee` `ic_result_cache` "hit".
- And at `:5246` / `:5248`: the warm listed outputs differ from a fresh-cache listed run. The warm run served the
  member's real IC; the fresh run shows sign 0 and 0 paired pairs.
- The other 10 tests stayed green.

Restore was done by the tool in the same run. `git status --porcelain` is empty, and `git hash-object` equals
HEAD's blob `49e02c6e7cb6db1a03c3fb099e772319052c4eb7`. No `git checkout` / `restore` / `stash` was used.

Same build:

- `/W4 /WX` is clean for every HEAD TU of the two targets, including `strategy_ic_absent.cpp`,
  `strategy_ic_composition.cpp` and the test TU. The only warnings are vendored databento `getenv` and clang-cl
  `/MP`.
- No eval TU was compiled, which confirms the lane's EXE-CUR claim: the `60094dcc` eval hunks are not in these
  targets.

## Evidence: mine vs the lane's

- **Mine:**
  - the teeth run above (AbsentMembers 10 / 11 on the mutated build, the 1 red being the mutated case);
  - merge plainness;
  - fixture and golden diffs;
  - 100-column and S-7 checks;
  - every file:line anchor;
  - the cache trace;
  - the SQL2-CLS literal scan (by grep, not by running the guard).
- **From the lane's logs only** (`abs-logs/`):
  - clean `AbsentMembers.*` 11 / 11 and the identity filter 53 / 53 on `p9-abs-b` (receipt: Source `7a748630`; ic
    exe `e488b79d...` = `p9-abs-a`'s, unchanged; tests exe `fc73c6e1...`). These hold at HEAD under EXE-CUR:
    `7a748630..5dae4f69` changes, among C++ / CMake, only eval files that these targets do not compile;
    `265bb427` is a fixture the test reads at run time.
  - the lane's teeth `p9-abs-c` (27 + 4 refusal cases and S1 red; I counted at least 5 failure lines per case in
    `teeth-p9-abs-c-gtest.log`).
- I did not rebuild a clean tree: TEST-1 allows one build, and that build was the mutated one.
- No pytest run (see the od3 row). No Release, canary, whole target or stage chain.

## Findings

| path:line | severity | problem | fix |
|---|---|---|---|
| `task-ABS-report.md` "New literals" (`:170-175`); literal at `atx-impl/src/strategy_ic_absent.hpp:5,34`, `strategy_ic_runner.hpp:107`, `strategy_ic_runner.cpp:1060` | Suggested (S-1) | The report says "no `classes.json` row needed". But the SQL2-CLS guard (`atx-engine/tools/test_research_store_classes.py`) scans every `atx.<name>/v<n>` literal in `atx-impl/src` C++, whether it is read or written. `atx.od3-flagged-members/v1` is in no class, domain or `legacy_allow` row of `classes.json` at HEAD (nor at root `fcb0200e`, nor at OD3P pool-21). So the guard is red on this head. OD3P already recorded the same literal as its "expected SQL2-CLS red" (`task-OD3P-report.md:206`, proposed class `cycle-spec`). | Root adds the class row at whichever of the OD3P / ABS merges comes first (SQL2-CLS practice; see "What root must check"). Correct the report line at the next lane touch. |
| `atx-impl/tests/strategy_ic_runner_test.cpp:5380-5423` | Suggested (S-2) | The signal-cache WRITE side of contract section 4 has no test with teeth. The only listed run on a cold cache (`fresh_listed`, own dir) is never read by a run without the list. So moving the blank before `cache_store` (a NaN signal under the real-signal key) would pass every test. The code is right today (store at `:312-316` inside `candidate_signal`, blank at `:533`). | Add a leg: listed run cold on a dir, then a plain run on that dir. The listed member's `signal_cache` should be "hit", and the outputs equal a fresh plain run. |
| `atx-impl/src/strategy_ic_runner.cpp:550` (`legacy_cached`) | Suggested (S-3) | The `--label-terminal` (missing-exit-v1) IC-result scope bypass is untested. No AbsentMembers test sets `--label-terminal`, so removing `&& !listed` there survives. The lane's S1 red came from `ic_cached` alone, as my teeth show. The P9 cycle does not send `--label-terminal` with the list (no such flag in pool-21's `research_roles.py` / `scripts/cycle`), so this is not live in the OD-3 read. | A shared-cache variant under `--label-terminal imputed-v1` asserting `ic_missing_exit_v1_result_cache` "miss" for the listed member, and that entry's bytes unchanged. |
| `atx-impl/src/strategy_ic_absent.cpp:66-71` | Suggested (S-4) | It re-implements `ic_detail::pinned_text` (`strategy_ic_library.cpp:53-59`: `hash_valid` -> `metadata_text` -> `sha256_hex` -> compare). The library, composition weights, theme registry, orientations and label role all use that helper. The only gain is the flag-specific messages. | Call `pinned_text` and map its two refusals to the flag's messages (or accept the shared ones). One pin-check rule (owner's modularity bar). |
| `task-ABS-report.md` "Equivalence" (`:99-121`) | Suggested (S-5) | ABS-2 asks the report to list every excluded JSON path. The report lists the eight dropped keys and the substituted values by identity, not by path. It omits the four warm-only keys (`verify_bytes`, `field_loads`, `peak_resident_fields`, `loaded_bytes`; shared-cache test only). | Copy the path enumeration of this review's ABS-2 section into the report at the next touch. Nothing is missing from the code. |
| `atx-impl/src/strategy_ic_absent.cpp:28,60`; `strategy_ic_runner.cpp:1147` | Suggested (S-6) | Style nits. `checked_list` lacks `[[nodiscard]]`. `const bool path = ..., pin = ...;` puts two declarators in one statement. `(pin?cfg.absent_members_sha256:cfg.absent_members_path)=value;` assigns through a conditional lvalue (legal, but hard to read). | Add `[[nodiscard]]`; one declarator each; an `if` / `else` assignment. |

Not findings (noted):

- `--plan-only` / `--cache-report` validate the list but print nothing new (Deviation 3). The contract names
  `summary.json` only.
- `orientations.json` and the recipe carry no trace of the list. A frozen TRAIN artifact made under the list is
  therefore indistinguishable from I'''s. That is the definition, so a validation-only pass applies I'''s TRAIN
  signs with or without the list.
- The run_ic refusals exit 1 and the repeated flag exits 2, which is the exe's existing split (Deviation 4).

## Quality verdict (owner priority: modular, reusable, performant C++)

**Sound.**

- **Modular:** one new metadata-only module, `strategy_ic_absent.{hpp,cpp}` (parse, refusals, record). One
  blanking line reuses the engine's own missing-member rule, with no second absence rule. The two cache-scope
  booleans are the only other runner change. S-3 lands as one private `Impl::weight_at`. S-4 is the one reuse gap.
- **Performant:**
  - The hot loop adds an O(1) mask read per candidate and one `std::fill` per listed member per role. There is no
    allocation; `signal_buffer` is reused.
  - The list parse is a linear scan per member against the library. It is bounded by N + 1 ids because a repeat
    or a non-candidate refuses, and it runs once before any payload.
  - The listed member's VM evaluation stays (ABS-5, P10).
- **Determinism:** members are kept as listed and the mask is in library order. There is no unordered container
  and no thread-order write; the blank precedes every pool use.
- **Safety:** no UB. Indexing is bounded (`absent` is sized to `lib.candidates`). The read is binary with a
  1 MiB bound. JSON is parsed without exceptions.
- **Build hygiene:** `/W4 /WX` is clean (my build). The lane adds 0 lines over 100 columns. The hygiene preset was
  not run (PCH build only).

## What root must check at merge

1. **Order:** ABS merges AFTER D3W `f0726acb`. ABS contains it through `58617960`, so merging ABS first would
   bring D3W in unreviewed by root's sequence. Merge `--no-ff`. Expected conflicts: none in the IC files.
   `eval_report.cpp` is B2-EVAL-KEEP, which is root's job at D3W's merge.
2. **SQL2-CLS** (S-1): add `atx.od3-flagged-members/v1` to a `classes.json` class at whichever of OD3P / ABS merges
   first. OD3P proposed `cycle-spec`; see OD3P's review on that class choice. Then re-run
   `atx-engine/tools/test_research_store_classes.py` (merge-affected). ABS writes no other new literal.
3. **ROOT-1a targeted checks:**
   - a Debug build of `atx-equity-strategy-ic,atx-impl-strategy-ic-tests` at the merge (next free root tag);
   - `AbsentMembers.*` (11);
   - the named identity filter `abs-logs/gtest-p9-abs-b-identity.filter` (53, including `CompositionRules.*`, which
     reads the regenerated fixture).
4. **`--list-rules --json` on root's exe:** `capabilities` is sorted, with `absent-members-v1` first. The fixture
   equals the exe output (`ListRulesJsonPinned`). State the exe's tag, SHA-256 and source (EXE-CUR).
5. **Flag-absent identity** (report "Flag-absent identity: procedure for root"):
   - argv: any IC pass without `--absent-members` (the tiny-world canary's u and w passes; the parents' identity
     runs), on the exe built at the merge vs root's exe before it;
   - byte-identical: every published file (`recipe.json`, `orientations.json`, `frozen_train_receipt.json`,
     `<role>_daily_ic.csv`, `<role>_planned_targets.csv`, `<role>_combined*`, sleeves);
   - `summary.json` and `<role>_candidates.jsonl` may differ only in `wall_seconds` / `stage_seconds` /
     `hash_seconds` (and cache paths);
   - intended non-pass changes: `--help` +6 lines; the envelope +1 capability.
   - The whole target, Release, canary goldens (1 / 4 workers) and the fixture's Python readers
     (`test_wave_kinds.py`, `test_composition_mom_volman.py`, `test_composition_theme_hedge.py`,
     `test_fit_composition_rule_plugins.py`) are run at the gate (TEST-1).
6. **After OD3P's merge:**
   - Run `scripts/tests/test_research_cycle_od3.py` (fakes).
   - Check that `exe_capabilities` (`scripts/cycle/phases.py:45-60`) on the real built exe returns a set holding
     `absent-members-v1`.
   - The cycle's plan on `scripts/specs/p9/od3/`, with a fake IC exe lacking the capability, must REFUSE (exit 2,
     before any step). The message must name `absent-members-v1`, the history roles and OD3P-1, and say the
     clean-source line is then "not read: no producer".
   - The same plan WITH the capability (fake offering it, then the real exe) must plan: each history role's u / w
     IC pass ends `--absent-members <spec path> --absent-members-sha256 <pin>` after `--candidate-cache` and any
     `--theme-registry`; the TRAIN anchor gets nothing.
7. **EXE-FREEZE:** ABS must be merged before the wave-3 P9-B0 re-pin. Any later relink of the IC exe redoes P9-B0
   and the re-pin first. If ABS is not merged and APPROVE at the gate, the clean-source line prints "not read: no
   producer" (OD3P-1). With this APPROVE, that applies only if the merge or the gate fails.
8. **For the PM:** ABS-3 (a)'s text ("a top-level key `absent_members`") does not describe this fixture. The one
   added line is the capability string. I judged (a) by its intent.

## Checked

- [x] `.agents/cpp/agent.md` section 10 applied to the diff. The hygiene preset was not run (TEST-1); includes
  compiled under PCH.
- [x] The diff stays inside the brief's files (cross-lane appends listed: CMake tail, detail-header comment, D1's
  capability table, the fixture).
- [x] The evidence in the report matches its claims: anchors, logs, receipts and EXE-CUR were re-checked. My teeth
  matched the prediction.
- Disk: my `build-equity` object tree (2.4 GB) was deleted by name. My receipt and logs `mega-p9-abs-d-*` are kept
  beside the lane's.
- No permission refusal.
