# Task T7 report — IC runner: pinned extra point-in-time fields in the DSL panel

Status: **DONE_WITH_CONCERNS** (implemented, fixtures written, desk-checked; not compiled or run, per lane
contract). Worktree `C:/atx-wt/pool-4`, branch `feat/mega-alpha-runner-fields-20260926`, base `9003f273`.

## Commits (in branch order)

| SHA | Subject | Root action |
|---|---|---|
| `ac91e600` | fix(research): v2 library fix round 1 before TRAIN measurement | **skip**: cherry-pick of root `6b4a21fe` (coordinator-sanctioned, for the real-v2 fixture) |
| `aa5f06dd` | feat(ic-runner): load pinned extra PIT fields into DSL panel [mega T7] | take |
| `2353bac8` | fix(ic-runner): VM-identity cache key, TRAIN-bound weights, pinned signs [mega T1 fix 1] | take; see `task-T1-report.md` § T1 fix round 1 |
| `4e018e58` | docs(alpha): point VM editors at the IC cache semantics version [mega T1 fix 1] | optional: comment only, but it recompiles every `vm.hpp` includer (~93 direct) |
| `d4992a32` | feat(ic-runner): refuse non-point-in-time research fields [mega T7] | take |

The T7 and T1-fix commits touch the same two files and must be applied in this order.

Files changed: `atx-impl/src/strategy_ic_runner.hpp`, `atx-impl/src/strategy_ic_runner.cpp` and
`atx-impl/tests/strategy_ic_runner_test.cpp` (+ the optional `atx-engine/include/atx/engine/alpha/vm.hpp`
comment). **There are no new files and no CMake changes.**

## Root build / test

- Build: `powershell scripts\atx-build.ps1 build atx-impl-strategy-ic-tests atx-equity-strategy-ic`
  - `atx-equity-strategy-ic` is the runner tool.
  - `atx-impl-tests` also globs this test file. Build it only if you want the full impl suite.
- Test: `powershell scripts\atx-build.ps1 -Ctest -R StrategyIcRunner`, or gtest filter
  `--gtest_filter=StrategyIcRunner.*`.

## Producer contract vs brief (followed the producer, `prepare_research_fields.py` + fix `c099cade`)

- Brief field names `iv_atm_30` / `hv_30` are stale. The real TRAIN names are si_shares, si_dtc, iv_atm_21d/63d/126d,
  earn_recent, shares_out, mktcap_lagged, size_grp, is_common, mkt_ret. The runner hard-codes no field names; it only
  enforces plain identifiers `[a-z_][a-z0-9_]{0,63}`.
- Brief: "a field list with name/source/clock/units/coverage". Each producer entry actually has:
  - `name`, `file`, `dtype "<f8"`, `layout "date-major"`, `shape [dates, instruments]`, `sha256`;
  - `sources` (a list, not `source`);
  - since c099cade, `point_in_time` and `non_pit_aspects`.
  Per-file bytes/sha are in top-level `files{<name>.f64: {bytes, sha256}}`. The role binding is
  `role{manifest_sha256, sessions_sha256, ids_sha256, member_sha256, dates, instruments, ...}`. The runner checks
  exactly these keys. No substantive mismatch.
- The real TRAIN dir `recent-fast-train-2020-2022-v1-fields-v1` (manifest `519fc9b2…`) predates the PIT flags, so the
  runner refuses it (see decision 7). It is also bound to the pre-repair role SHA. It must be regenerated with the
  c099cade producer against the repaired roles.

## What changed (`strategy_ic_runner.cpp` line numbers at `d4992a32`)

- **Config/CLI** (hpp `IcRunnerConfig`; cpp `dispatch_ic` :1407): new options `--train-fields DIR`,
  `--train-fields-sha256 SHA`, `--validation-fields DIR`, `--validation-fields-sha256 SHA`. The SHA pins
  `DIR/manifest.json`. `run_ic` rejects with `bounded config` if:
  - a directory and its SHA are not given together, or
  - `--validation-fields` is given without `--validation`.
- **Library contract** (`library` :399). The library must declare close/raw_close/volume plus any plain-identifier
  extras. Duplicate declared names are now refused (before, a set silently collapsed them).
  - Each candidate's `extra_fields` = the non-base names in its compiled `Program::fields`. That list is exactly what
    the VM resolves by name.
  - `Library::extra_fields` is the sorted union of those (the only fields ever loaded).
  - `Library::declared_extra` is every declared extra.
  - Referencing an undeclared field refuses: `undeclared DSL field: <name>`.
- **Binding before any payload, also under `--plan-only`** (`bind_fields` :525, called right after each `admit`):
  - Checks, in order:
    - hash pin (`pinned_json`);
    - `schema == atx.research-role-fields/v1` and `status == complete`;
    - `role.manifest_sha256` == the pinned role SHA;
    - `role.sessions_sha256` / `role.ids_sha256` == the role manifest's `files` receipts;
    - `dates` / `instruments` equal.
  - Every entry must be a valid identifier, not a base name, and unique. It also needs `file == <name>.f64`,
    `<f8`, `date-major` and `shape == [d, n]`. Its SHA must be a valid hash equal to the `files` receipt, with bytes
    `== d*n*8`. Every entry also carries consistent PIT flags.
  - Every declared extra must be in the manifest, and must be point-in-time.
  - For a scored role, each referenced field's payload extent is stat'ed. No payload byte is read.
  - A declared extra with no pinned manifest refuses for a scored role.
  - In validation-only mode TRAIN is unscored and needs no fields. If `--train-fields` is given, it is still bound
    (metadata only).
- **Loading** (`load_fields` :925; start of `score_role` :1071):
  - Only referenced fields load, in sorted-name order, before the role payload, so a tampered field refuses before
    `read_strategy_role` opens anything.
  - Extent check plus streamed SHA256 through `load_pinned_f64`. That is the T1 `cache_load` reader generalized, with
    unchanged cache messages.
  - ±inf is refused; NaN is allowed.
  - An `IC fields-loaded role=… fields=… seconds=…` progress line is written only when fields are loaded.
- **DSL panel** (`dsl_panel` :940).
  - `Panel::create_borrowed` over the role panel's own base column spans and the loaded extras. **No column is ever
    copied.**
  - The only owned buffer is a 1 B/cell presence mask copied from `role.panel.in_universe`. `LoadField` therefore
    NaNs extras exactly where it NaNs base fields.
  - `set_cross_section_mask(role.decision_member)` is unchanged.
  - Without extras the VM gets `role.panel` itself, so the default path is identical by construction.
  - Lifetime order: `extras` → `role` → `field_panel` → … → `vm`. Both extras and panel are released right after the
    candidate loop (with the VM), before `composition.finish()`.
- **Memory admission** (`admit`, :482):
  - Adds `cells × (8·k + 1)` when k > 0 referenced fields: 8 B/cell per field plus the 1 B/cell presence mask.
  - Nothing is added when k = 0, so default `required_bytes` / `admitted_working_bytes` are unchanged.
  - Unreferenced manifest fields are never admitted, opened or stat'ed.
- **Candidate cache key** (`cache_key` :689, `cached_payload_sha` :740, `cache_store`):
  - A base-only candidate keeps `ROOT/<role-manifest-sha>/`.
  - A candidate that references any extra lives in `ROOT/<fields-manifest-sha>/`. That manifest pins exactly one
    role, and path length is unchanged.
  - Field entries' sidecars record `fields_manifest_sha256` and `research_fields`, and lookup requires them.
  - A base lookup refuses any sidecar that names a fields manifest.
  - Result: a changed field payload is a clean miss into a new directory, never a stale hit, and base-only entries
    keep their keys.
  - `ROOT` is the T1-fix VM-identity root. It is `DIR` itself for the dev build; see the T1 report.
  - Plan/summary report `fields_directory` next to `directory`.
- **Records**, all absent unless fields are pinned, so default bytes are unchanged:
  - **recipe.json** `research_fields`: `{schema, manifest_sha256: {train?, validation?}, loaded: [...], semantics}`.
    It is present whenever any fields manifest is pinned, even if nothing is referenced (brief: "changes only when
    fields are supplied").
  - **summary.json**: top-level `research_fields` (same block).
  - **role result** `research_fields`: `{manifest_sha256, directory, loaded, files{name: {bytes, sha256}},
    loaded_bytes}`, plus `stage_seconds.fields_load`.
  - **plan output** `research_fields`: `{loaded, declared, roles[...]}`.
  - **saved combined manifest**: `research_fields_manifest_sha256`. Target/NAV replay tolerate extra keys and their
    semantics strings are unchanged.
- **Frozen-TRAIN resume** (`expect_frozen_fields` :200, called in `frozen_train` before the exact recipe comparison):
  - The source recipe's `research_fields` is rebuilt from the library and pins.
  - The TRAIN pin comes from the source, because the TRAIN payload is never opened. If `--train-fields` is re-given
    it must match.
  - A validation pin must equal this run's, like role pins.
  - A library that declares extras against a source without `research_fields` refuses.

## Decisions (small ambiguities)

1. **Declared vs referenced.** Declared extras must all be in each scored role's manifest (coordinator ruling), but
   only referenced ones are loaded and admitted.
2. **Recipe recording** triggers when fields are supplied, not when fields are loaded.
3. **Tampered payload timing.** A tampered same-size field payload is caught by the streamed hash at load, after
   output-dir creation (`summary.status=failed`) but before the role payload is opened. Plan-only mode stats extents
   only, like role payloads and cache entries.
4. **Values.** Field values may be NaN or finite. Infinite values are refused (producer contract).
5. **Cache key** uses the manifest SHA (as the brief asks), not per-field SHAs. A regenerated manifest re-evaluates
   every field candidate once.
6. **Field order.** Extras are ordered by sorted name after the base three. The VM resolves by name, so order is
   irrelevant to results.
7. **PIT (coordinator requirement, `d4992a32`, `non_pit_aspects` :506).**
   - Every manifest entry must carry boolean `point_in_time` and array `non_pit_aspects` (strings), consistent
     (true ⇔ empty).
   - Otherwise the whole manifest refuses: `… lacks point_in_time/non_pit_aspects flags` or
     `… point_in_time contradicts non_pit_aspects`.
   - I chose **refuse** for pre-flag manifests. Their absence cannot be shown safe (the old dirs list
     is_common/mktcap_lagged/size_grp unmarked), and those dirs are bound to pre-repair role SHAs anyway.
   - A library that declares (so a fortiori loads) a `point_in_time=false` field refuses:
     `library field 'is_common' is not point-in-time in the pinned train fields manifest (non_pit_aspects: values);
     refusing look-ahead`. There is no override flag.
   - Non-PIT entries in a manifest are harmless while undeclared.
8. **No hard-coded role SHAs.** The only SHA constants are T1's legacy *engine* commit SHAs.

## Fixtures (`strategy_ic_runner_test.cpp`)

Brief (a)–(e):
- **(a)** `ExtraFieldsResolveByNameAndMatchHandComputedSignals` :860
  - `si_shares / volume` and `volume + mkt_ret`: the raw VM payloads (read from the cache) are compared cell by cell
    with scalar oracles, NaN where the field is not visible.
  - Checks recipe/summary/role/combined-manifest records and the fields-directory sidecars.
  - Base entries stay in the role directory with no fields key.
  - The 4-candidate blend equals the centered rank.
- **(b)** `UnreferencedFieldsAreNeverOpenedOrAdmitted` :913
  - Unreferenced payloads are deleted and the run still succeeds.
  - `loaded` lists only the referenced field.
  - Plan `required_bytes` of a same-shape library (`raw_close / volume`) + `D·N·9` equals the field library's.
  - Pinned but unreferenced fields admit nothing.
- **(c)** `FieldBindingRefusalsPrecedeRolePayloadAndOutput` :951
  - Role `close.f64` is deleted.
  - Each case below refuses in both plan-only and real mode, with no output and empty progress:
    - declared field absent from the manifest;
    - undeclared DSL field;
    - dotted declared name;
    - no TRAIN or validation manifest pinned;
    - unpaired option;
    - validation manifest pinned as TRAIN's;
    - wrong pin;
    - re-pinned lies (sessions sha, shape, schema);
    - truncated payload.
  - A same-size bit flip passes plan-only, then refuses on `research field payload SHA256 mismatch: si_shares.f64`
    before the deleted role payload, and the file is left untouched.
- **(d)** `AbsentFieldOptionsLeaveRecipeAndOutputsUnchanged` :1027
  - The default recipe key set is asserted exactly.
  - No new summary, role, combined or plan keys; no fields progress line.
  - With pinned-but-unreferenced fields: IC/targets/blend bytes are identical, and the recipe differs only by
    `research_fields`.
- **(e)** `CandidateCacheKeyCoversFieldsManifestOnlyForFieldCandidates` :1068
  - Entries written by a base-only run keep hitting when the library grows a field candidate.
  - The field candidate misses into `ROOT/<fields-sha>/`, then hits when warm.
  - A new fields manifest gives a clean miss into its own directory. The old entry and the base sidecar bytes are
    untouched.
  - A field entry copied under another manifest's directory refuses.

Coordinator additions:
- `LibraryDeclaringMktRetRunsOnlyWhenFieldsSupplyIt` :1114: no fields → refuse; a manifest without `mkt_ret` →
  refuse; with it → runs and loads `["mkt_ret"]`.
- `RealV2LibraryDeclaringMktRetPlansOnlyWithPinnedFields` :1130
  - Uses the real `atx-impl/strategies/price_volume_ic96_v2.json` (b871743e) in plan-only mode.
  - Asserts the declared set is {close, raw_close, volume, mkt_ret}.
  - Score start is moved to 450 (metadata-only re-pin) so the longest lookback fits (my estimate ≈317 ≤ 387).
  - Unpinned → refuse; pinned `mkt_ret` → all 96 DSLs compile and `loaded == ["mkt_ret"]`.
- `FrozenTrainResumeBindsResearchFieldPins` :1159
  - Validation-only resume needs only the validation fields and reproduces the source's validation bytes.
  - A moved validation pin, a different re-given TRAIN pin, or a missing validation manifest each refuses.
- `NonPointInTimeFieldsAndUnflaggedManifestsAreRefused` :1193 (PIT requirement): non-PIT entries are OK while
  undeclared. A used or declared non-PIT field refuses, and so do an unflagged or a contradictory manifest.

The T1-era tests were updated in the T1 fix commit (see the T1 report). The fixture's fields manifests mirror the
producer's keys, including the PIT flags.

## Concerns

1. **Not compiled.** Desk-check friction points:
   - nlohmann `Json::array({d,n})` equality on the `shape` field;
   - `ATX_TRY` used inside loop bodies (it expands to several statements; every use is braced);
   - the `Panel::field_name(usize)` overload in `dsl_panel`;
   - gtest printing of `Json` in `EXPECT_EQ`.
2. **The real-v2 fixture** depends on v2's compiled lookback being ≤ 387 and on all 96 DSLs compiling in the runner
   (≤ 64 slots, one root). If it fails, the error names the cause (warmup, slots or compile), which is a genuine v2
   finding rather than a T7 defect. The v2 non-DSL constraints (ids, families, sign policy, horizons, uniqueness)
   were verified in Python.
3. **T9 / cache consumers.**
   - Field-referencing candidates' cache entries are under `ROOT/<train-fields-manifest-sha>/`, not
     `ROOT/<train-role-sha>/`. The T9 fitter currently reads only `DIR/<train_sha>/<id>.json`, so for v2's 26
     `mkt_ret` candidates it must also look in `DIR/<fields-sha>/`. The sidecar carries `fields_manifest_sha256` and
     `research_fields`.
   - `ROOT` equals `DIR` only for the dev build's identity (see the T1 report).
4. **The v2 fixture moves score start in metadata only.** Real TRAIN has score_begin 399, so v2 needs lookback
   ≤ 336 there. Root should run `--plan-only` on the real roles first.
5. **The recipe changes when fields are pinned but unused.** That follows the brief. If root prefers "only when
   loaded", it is a one-line change in `run_ic`/`expect_frozen_fields`.
