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

## Fix round 1

Status: **DONE** (root cause found by reading the code; the fix is not compiled here). Commit `c942afea`
test(ic-runner): stop iterating destroyed JSON temporaries [mega T7 fix 1]. It changes only
`atx-impl/tests/strategy_ic_runner_test.cpp`. The runner itself is not affected.

### Root cause

Four new fixtures used a range-for over a member of a temporary:
`for (const auto& x : read_json(path).at("key"))`.

C++20 lowers this to `auto&& __range = read_json(path).at("key");`. That binds a reference *into* the
`Json` returned by `read_json`. The temporary is destroyed at the end of that full-expression: no
lifetime extension applies through a member-function call, and only C++23 P2718 fixes this. The loop
then walks freed nlohmann storage. The first `row.at("sign")` / `role_result.at(...)` hits a node whose
type tag now reads as a number, so it throws `type_error.304 "cannot use at() with number"`. This is
undefined behaviour, so it could equally have crashed or passed.

Exact throwing lines at `d4992a32` (identical in root's imports, since the test file was not otherwise
changed):

| Test | Line | Throwing statement |
|---|---|---|
| ExtraFieldsResolveByNameAndMatchHandComputedSignals | :907 | `for (row : read_json(output/orientations.json).at("candidates")) EXPECT_EQ(row.at("sign"),1)` |
| LibraryDeclaringMktRetRunsOnlyWhenFieldsSupplyIt | :1127-1128 | `for (role_result : read_json(output/summary.json).at("roles")) … role_result.at("research_fields")` |
| PinnedSignOppositeToIcOrientationFlipsOnlyThatBlendContribution | :1254 | `for (row : read_json(signed/orientations.json).at("candidates")) … row.at("sign")` |
| CandidateCacheIsScopedByVmIdentityAndRefusesForeignSidecars | :1303-1304 | `for (role_result : read_json(other/summary.json).at("roles")) … role_result.at("candidate_cache")` |

### Fix

Each document is now parsed into a named local (`orientations`, `summary`, `other_summary`) and the
loop iterates that local.

Checked for any remaining instances:
- No other range-for over a temporary remains in the test file.
- No `auto&` binds to `read_json(...).at(...)`.
- The runner has none of either.

Function-argument uses such as `EXPECT_EQ(read_json(...).at(k), v)` and
`stable_role(read_json(...).at(..))` are safe: the temporary lives until the end of the full-expression,
and `stable_role` copies.

### Cache path rule for the TRAIN weight fitter (T9/T11)

Inputs:
- `DIR`: the runner's `--candidate-cache`.
- `R`: the TRAIN role manifest SHA-256 (`--train-sha256`).
- `F`: the TRAIN fields manifest SHA-256 (`--train-fields-sha256`, the sha of `<fields dir>/manifest.json`).
  It exists only when fields were pinned.
- `I`: the runner build's VM identity.

1. **Root.** `I` is `dslvm<version>_<compiler><fp-flavor>`, e.g. `dslvm1_clang18.1`. The run publishes it as
   `summary.json roles[i].candidate_cache.vm_identity` (plan-only: `candidate_cache[i].vm_identity`), and every
   new sidecar records it as `vm_identity`.
   - `ROOT = DIR` if `I == "dslvm1_clang18.1"` (root's dev build).
   - Otherwise `ROOT = DIR/<I>`.
2. **Entry directory for candidate `id`.**
   - If the candidate's compiled DSL reads any field other than `close`/`raw_close`/`volume` (a "field
     candidate"; v2's `mkt_ret` users), the entry directory is `E = ROOT/<F>`.
   - Otherwise `E = ROOT/<R>`.
3. **Files.** `E/<id>.json` (sidecar, the commit record) and `E/<id>.f64` (date-major little-endian f64,
   role dates × instruments).
4. **Checks.** The sidecar must have:
   - `role_manifest_sha256 == R`;
   - `fields_manifest_sha256 == F` for a field candidate, and the key absent for a base candidate;
   - `vm_identity == I` (a keyless sidecar is valid only under `I == dslvm1_clang18.1` and with
     `engine_git_sha` ∈ {`429cbe43…`, `6d85ac2a…`});
   - plus the existing schema, id, `dsl_sha256`, `eval_mode`, `layout` and geometry checks, and
     `payload_sha256` == the sha of the `.f64`.

**Recommended implementation, with no DSL parsing.** Read `ROOT/<R>` and `ROOT/<F>` straight from the
TRAIN run's `summary.json`:
- `roles[train].candidate_cache.directory` is `ROOT/<R>`.
- `roles[train].candidate_cache.fields_directory` is `ROOT/<F>`. It is present only if the library
  references extra fields.

For each candidate, open `directory/<id>.json`, else `fields_directory/<id>.json`. Exactly one should
exist. Where it was found must agree with the sidecar's `fields_manifest_sha256` presence (and value `F`).
The sidecar's `research_fields` lists the extra fields that candidate read.

## Fix round 2

Branch `feat/mega-alpha-runner-fields-fix2-20260927` in pool-4, commits `7c870053` (fix) and `2dfadb36` (test (d)
tightened to the new progress lines) on root `e885687b`.
Not built or run here (lane rule): root builds `atx-impl-strategy-ic-tests` (and `atx-equity-strategy-ic` for the
binary), then runs `--gtest_filter=StrategyIcRunner.*` (ctest: `-R StrategyIcRunner`). No CMake change:
`ATX_IMPL_TESTS_DIR` (`atx-impl/tests/CMakeLists.txt:21`) is already defined for both test targets, and the
tripwire takes the repo root as its `parent_path().parent_path()`.

### I1: per-candidate field residency

- **Schedule (`FieldPlan`, built in `library()`).** Candidates must run in library order because
  `IcComposition::add` accumulates in that order, so the schedule is fixed per library:
  - `needs[k]` is candidate k's extras bitmask;
  - `capacity` is the largest `popcount(needs[k])`;
  - `planned[k]` is the resident set while k runs, chosen by Belady's MIN: a field with no later reader is
    dropped, and a load at capacity evicts the resident field whose next read is farthest (lowest index on
    ties).
  - It is optimal for the number of loads at that capacity.
- **Runtime (`FieldResidency`, declared after `role` and before `pool`/`vm`).**
  - `enter(k)` releases whatever `planned[k]` excludes.
  - On a cache miss only, `panel_for(k)` loads candidate k's missing fields, streaming the pinned SHA, and
    (re)builds the borrowed DSL panel over the resident set; with none resident it returns `role.panel`.
  - Any change to the resident set destroys the VM first, so a live Engine always borrows the current panel.
  - The resident set is always a subset of `planned[k]`, so at most `capacity` columns are held. A warm (all
    hit) run loads nothing.
- **Fail-fast order is kept.** `verify_fields` streams the SHA of every referenced field before the role payload
  opens, so a tampered field still refuses first. Loads hash the bytes again as they land.
- **Admission.** Extras now cost `cells × (8·capacity + 1)` instead of `cells × (8·referenced + 1)`.
- **v3 (`pv_fields_ic121_v3.json` 5d164ea1).**
  - 8 extras referenced; candidates use 0 / 1 / 2 extras in 70 / 38 / 13 cases.
  - capacity 2, 11 planned loads, peak 2 columns (instead of 8).
  - Streaming 11 × 52 MB per role is seconds of I/O.
- **Expected v3 `required_bytes` on real TRAIN** (1155 × 5627, 8 slots): root's recorded figures minus
  cells × 48 = 311,960,880 B.

  | Workers | Before (root, recorded) | After | Headroom to 1536 MiB |
  |---|---|---|---|
  | 4 | 1,656,728,202 B (1580.0 MiB) | **1,344,767,322 B (1282.5 MiB)** | 253.5 MiB |
  | 2 | 1,627,427,722 B (1552.0 MiB) | **1,315,466,842 B (1254.5 MiB)** | 281.5 MiB |
  | 1 | (1524.1 MiB) | 1,286,166,362 B (1226.6 MiB) | 309.4 MiB |

  Each additional compiled VM slot adds about 49.6 MiB (9 slots at w4 is 1332.1 MiB; 10 slots is 1381.6 MiB).
- **`--workers` is not the lever.** Two workers cost only 29,300,480 B (about 28 MiB), which is why w2 was
  still refused. The eight resident columns (about 397 MiB) were the problem, so the fix is residency and v3
  can run at w4.
- **Records.**
  - Role `research_fields` adds `resident_capacity`, `planned_loads`, `field_loads` and
    `peak_resident_fields`.
  - `loaded_bytes` is now `field_loads × cells × 8`, the bytes actually read, so it is 0 on a warm run.
  - `stage_seconds` adds `fields_verify`; `fields_load` is the sum of load times.
  - The plan-only `research_fields` adds `resident_capacity` and `planned_loads`.
  - The recipe is unchanged.
- **Progress lines.**
  - `IC fields-loaded …` is replaced by `IC fields-verified role=R fields=… resident_capacity=C planned_loads=L`.
  - New: `IC field-load role=R field=F candidate=ID`, `IC field-release role=R field=F` and
    `IC cache-preflight role=R ready=H/N`.
  - No tool in the tree parses the old line.
- **Output bytes are unchanged.**
  - The Engine resolves fields by name on every evaluate (`vm.hpp:1128`), and LoadField masks with the
    borrowed panel's presence, which is copied from the role. So a subset panel yields the same bits.
  - The blend order is untouched.
  - Fixture `ExtraFieldsResideOnlyWhileReferencedAndReloadExactly`:
    - hand schedule with a forced eviction and reload (mkt_ret loaded twice): 4 loads, peak 2;
    - exact scalar oracles on every raw field signal;
    - a base candidate evaluated while `si_shares` is resident matches base-only bytes;
    - uncached, cold and warm runs are byte-identical on all `exact_outputs()`, and the warm run has 0 loads.

### Real-data refusal at `--workers 1` (`metadata missing/over1MiB`)

This is not a workers-specific path.
- At w4 and w2, admission refuses first, at the end of `admit`. At w1, admission passes and the next step,
  `bind_fields`, opens `<--train-fields>/manifest.json` through `metadata_text`, which refuses a missing or
  over-1 MiB file.
- Both real manifests are small: role 22.8 KB, fields 36.9 KB. `repair_cells.csv` is never read.
- So the file it looked for did not exist. The only way that happens with a valid directory is
  `--train-fields` naming the manifest FILE (`…-fields-v2/manifest.json`), which makes the runner open
  `…/manifest.json/manifest.json`. The option takes the DIRECTORY; its `-sha256` pins `DIR/manifest.json`.
- **Fixes.**
  - `bind_fields` now refuses a non-directory by name: `--train-fields must name the fields directory (the one
    holding manifest.json), not a file or missing path: <arg>`. Fixture: `FieldsOptionMustNameTheDirectory`.
  - `metadata_text` refusals now carry the path and distinguish "missing or unreadable" from
    "empty or over 1 MiB (N B)".
- If root's command already passed the directory, the new message names the exact file that was missing.

### I2: VM source tripwire

- **Pin.** `dsl_vm_sources` (29 repo-relative paths, beside `dsl_vm_semantics_version`) plus
  `dsl_vm_sources_sha256 = 51bc0b2e08b27b1c025759e8c11755ef499df8ef9a723b33116e7164c770d67e`.
  - Paths: the runner TU's include closure under `atx/engine` minus `factory/` (IC scoring, not signal bits);
    the TUs those headers declare (`src/alpha/{bytecode,dag,lexer,panel,parser,registry,subtree_cache,typecheck}.cpp`);
    and the role reader `data/strategy_data.{hpp,cpp}`.
  - Exposed through `ic_cache_vm_identity()` (hpp).
- **Digest.** SHA-256 of the concatenation over paths in order of `path\n<len>\n<text>`, where the text is
  CRLF→LF normalized.
  - The same value is computed from git blobs and from the CRLF working tree.
- **Test `StrategyIcRunner.VmSourcesPinnedToSemanticsVersion`.**
  - On a mismatch it fails with "VM sources changed: bump dsl_vm_semantics_version and update the pinned
    hash", and gtest prints the new digest to paste.
  - It also fails if a listed file `#include`s an `atx/engine/...` header that is not listed, so the list
    cannot fall behind the closure.
- **Action.** If root's engine sources differ from `e885687b` at merge, the pinned hash must be re-derived
  there. That is a decision, not a formality.
- **Out of scope for the guard.** `atx-core` (types, error, hash) and the runner's own compile or eval-mode
  code stay covered only by the version comment.

### Minors

Done:
- **M2.** Presence masking: `ExtraFieldsAreMaskedByRolePresence`, with holes where present=0 but the field
  file is finite.
- **M3.** Base-only candidate bytes are equal in a fields run: test (a), plus `base_mid` on the DSL panel in the
  residency fixture.
- **M4.** The header cache-layout comment now reads `DIR[/<vm-identity>]/<role-or-fields-sha>/`.
- **M5.** With both manifests pinned, each declared extra's definition must match:
  - compared keys: `units`, `clock`, `staleness`, `source_columns`, `definition`, `point_in_time`,
    `non_pit_aspects`, `plausibility{min,max,inclusive,rule}`;
  - role-specific keys may differ;
  - fixture: `FieldDefinitionsMustMatchAcrossRoles`.
- **T1 M2.** Fail-fast sidecar preflight (`cache_plan`) runs before the role payload. The extent refusal names
  `<id>.f64`, matching the load path.
- **T1 M6.** `CandidateCachePreflightRefusesBeforeRolePayload` covers: a corrupt last sidecar refused before any
  eval; a truncated payload refused up front; a cache path that is a file; a `con` candidate id.

Not done:
- **M1** (validation weights tied to the orientations SHA): the root-ruled contract; changing it needs root's
  ruling.
- **M6** (vm.hpp comment): already imported by root. Touching vm.hpp now would also move the new pin.
- **M7** (report overstated lookup): correction only. `cached_payload_sha` matches `fields_manifest_sha256`,
  not `research_fields`; this is harmless because `dsl_sha256` fixes the fields.
- **T1 M3** (shared hex helper): it belongs in atx-core, outside this lane.
- **T1 M5** (`ASSERT_DOUBLE_EQ` where exact): the remaining uses compare ranked blends where DOUBLE_EQ is the
  intended tolerance. The new oracles use exact `same_value`.
- **T1 M6, unit-level composition refusals:** `strategy_ic_composition` is T1's file and not touched here.

### Fixture changes

- (a) `ExtraFieldsResolveByNameAndMatchHandComputedSignals` now asserts:
  - the `fields-verified` line;
  - the load, release and load order;
  - capacity 1, 2 loads, peak 1;
  - `fields_verify`;
  - base payload equality.
- (d) asserts that no `fields-verified`, `field-load` or `field-release` line appears without fields.
- New tests:
  - `ExtraFieldsResideOnlyWhileReferencedAndReloadExactly`
  - `ExtraFieldsAreMaskedByRolePresence`
  - `FieldDefinitionsMustMatchAcrossRoles`
  - `FieldsOptionMustNameTheDirectory`
  - `CandidateCachePreflightRefusesBeforeRolePayload`
  - `VmSourcesPinnedToSemanticsVersion`
- Test helper `role()` gained an optional `holes` list.

The cache path rule (Fix round 1) is unchanged.
