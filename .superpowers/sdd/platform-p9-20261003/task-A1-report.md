# Lane A1 report

## Outcome
DONE. Shipped: the K-P9-1 field registry and its loader; the one builder entry, with the four shims reduced to thin deprecated wrappers and holdings moved onto `FIELD_MODULES`; the DEC-5 freeze; runtime versions in the reuse key and the manifest, plus the IMPORTS-closure test (FD-2); the Python-side seal fixes (FD-5). The conftest 2025 bind stays, per the PM's ruling A1-C (ruled deviation, see Deviations).

## Branch / SHA
`feat/p9-a1-20261003` @ `aa783bb1` (code and tests). This report is the next commit on top. All work is committed and the leased tree is left clean.

| task | commit | content |
|---|---|---|
| (1)+(2)+(4) code, (5) | `a2d783a8` | registry json and loader; entry; shims; holdings module; runtime pins; seal refusal; key rename; IMPORTS completion; fixture regenerated; renamed-key updates to 4 existing tests |
| (1) test | `b39bdb83` | `test_field_registry.py` (17 tests) and `atx-engine/tools/.gitattributes` (pins registry bytes) |
| (3) | `a452581e` | `test_no_new_python_builder.py` (allowlist with 18 paths, 92 frozen Python fields) |
| (4) tests | `1e7516e7` | `test_prepare_research_fields_reuse_keys.py`, `test_field_module_imports.py` |
| (5) test | `aa783bb1` | `test_prepare_research_fields_seal.py` (fresh interpreter, repository window) |
| (6) | none | moved to A2 by ruling P5 (`engine.py:13` dead test reference) |

## Frozen base / lease
base_sha=d7c1c520c3caa162ef453349b256669ce8de3c80; worktree=C:\atx-wt\pool-12; lease_name=pool-12;
lease_run_id=p9-a1-20261003; heartbeat_id=p9-a1-hb; keeper_pid=26400; keeper_process_started_utc=per root's pool record.

## Acquisition receipt
Root leased the pool at R0-0 (`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/pools.md`, row A1: `C:/atx-wt/pool-12`, branch `feat/p9-a1-20261003`, run `p9-a1-20261003`, heartbeat `p9-a1-hb`, HEAD `d7c1c520c3ca...`, keeper 26400 alive). This lane did not lease or release.

## Files changed
New:
- `atx-engine/tools/field_registry.json` (K-P9-1; 92 rows, 200282 bytes, sha256 `6c56b739ac232b297d82987b60202d94b647206da0c37b2c51f36eb4e43e3e51`)
- `atx-engine/tools/field_registry.py`
- `atx-engine/tools/.gitattributes` (`field_registry.json -text`)
- `atx-engine/tools/test_field_registry.py`
- `atx-engine/tools/test_no_new_python_builder.py`
- `atx-engine/tools/test_field_module_imports.py`
- `atx-engine/tools/test_prepare_research_fields_reuse_keys.py`
- `atx-engine/tools/test_prepare_research_fields_seal.py`

Modified:
- `atx-engine/tools/prepare_research_fields.py`:
  - entry dispatch `main` → `field_registry.entry` / `parse_and_run`;
  - `run(**module_kwargs)`;
  - LATE-module hooks;
  - `runtime_versions` and `runtime_mismatch`;
  - `require_research_seal` in `load_prior`;
  - `published_checks` (rename at publish);
  - `engine_produced`;
  - holdings as `FIELD_MODULES.append(_holdings.bind(globals()))`.
- `atx-engine/tools/research_fields_holdings.py`: `register()` and its run/main carrier are replaced by `bind(ns)` and `HoldingsFieldModule` (LATE, `merge_reuse`, `finish`). Producer closures are unchanged.
- `atx-engine/tools/prepare_research_fields_{draft,xdata,ohlc,ydata}.py`: DEPRECATED. `register` = `field_registry.bind_modules(ns, DRAFT_MODULES)`; `main` = `register(vars(builder)); builder.main(argv)`.
- `atx-engine/tools/research_fields_v9.py`: `IMPORTS["v9_nt"]` adds `SEC_CLOCK`, `STAGES`.
- `atx-engine/tools/research_fields_deals.py`: IMPORTS adds sec `SEC_CLOCK`, `STAGES` and v9 `NT_PRESENCE_DAYS`, `NT_PRESENT`, `PERIODIC_FORMS`.
- `atx-engine/tools/code_fingerprint.py`: `cross_module_reads(source, entries, local_modules, orchestration)`.
- `atx-engine/tools/conftest.py`: docstring only (ruling A1-C; the bind is unchanged).
- `atx-engine/tests/fixtures/research_fields/make_research_fields_fixture.py`: `CODE_KEYS` += `runtime_versions`; asserts the repository window.
- `atx-engine/tests/fixtures/research_fields/expected/manifest.normalized.json`: regenerated. The only change is the key rename at its 2 occurrences; no payload moved.
- `atx-engine/tests/fixtures/research_fields/test_research_fields_fixture.py`: asserts `rows_sealed_dropped == 1`, legacy key absent, seal 2024-01-01, no `runtime_versions`.
- Key-rename updates:
  - `atx-engine/tools/test_prepare_research_fields.py`
  - `atx-engine/tools/test_prepare_research_fields_sic.py`
  - `atx-engine/tools/test_research_fields_v8_quarters.py`
  - `atx-engine/tools/test_research_fields_v9_earn.py`

## K-P9-1 schema shipped (exact; A2 reads this)
File: `atx-engine/tools/field_registry.json`.

**Byte form.** `field_registry.dump`: `json.dumps(doc, indent=2, sort_keys=True, allow_nan=False) + "\n"`, UTF-8, LF. `.gitattributes -text` keeps those bytes on checkout.

**Top level.** Exactly three keys:
```
{"dtype_rule": "<DTYPE_RULE text>", "fields": [<row>, ...], "schema": "atx.field-registry/v1"}
```

**Row.** Exactly the 12 `ROW_KEYS` below, no others. Rows appear in registry order, which is fields-manifest order: the builder's `ALL_FIELDS` order, then LATE-module fields (holdings last).

| key | type | value |
|---|---|---|
| `name` | non-empty str | unique within the file |
| `kind` | `"python"` \| `"engine"` | All 92 rows ship as `python` (P5); A2 flips si_shares, si_dtc and vol_126. P6: python = legacy Python producer; engine = never reused by the Python AST path |
| `builder` | non-empty str | python row: the Python module that computes it, no `.py` (`prepare_research_fields` for builder-dict fields). engine row: the C++ BuilderKind id (A2) |
| `dtype` | `"f64"` \| `"group"` | Declared by `dtype_rule`: `group` iff the spec units start with `categorical code`. The 5 group rows are grp_sic2, grp_ff12, grp_ff49, grp_ff12f49 and ea_time_of_day |
| `point_in_time` | bool | 89 true, 3 false |
| `spec_text` | object | Exactly `units, clock, staleness, source_columns, definition, point_in_time, non_pit_aspects, domain`, i.e. the builder's `spec_definition` keys at the declared fundamentals lag. `definition` and `domain` may be null; `domain` is a list of float when present |
| `formula_sha256` | 64 lower-hex \| null | The builder's `formula_id(name, spec_text)`; non-null in all 92 rows |
| `requires` | list[str] | Each name must be an earlier row (12 rows non-empty) |
| `options` | object | Python rows: `{"group": <producer group --reuse fingerprints>}` |
| `sources` | list[str] | Input names the producer reads (`run()` keywords / module option destinations). Informational |
| `first_session` | str \| null | null in all rows (not declared yet) |
| `owner` | str | The registration mechanism the row came from (see below) |

`owner` values:
- `builder:FIELDS`, `builder:ISSUER_FIELDS`, `builder:SV_FIELDS`;
- `builder:FIELD_MODULES` (sec, price, v8, holdings);
- `shim:prepare_research_fields_{draft,xdata,ohlc,ydata}`;
- the 3 ported fields carry `; engine-shim:prepare_research_fields_engine` appended.

**Engine-row module resolution.** `python_modules`:
- An engine row's `builder` is never imported.
- Its Python fallback module comes from `owner`'s first mechanism. `shim:X` gives `X.DRAFT_MODULES`; anything else means the builder binds it itself.

**Validation (`validate`) refuses:**
- a wrong `schema`, or a missing `fields` list;
- a row whose key set is not exactly `ROW_KEYS`;
- an empty or duplicate name;
- `kind`, `dtype` or `point_in_time` outside the domains above;
- a non-string `builder` or `owner`;
- a bad `formula_sha256`;
- `requires` or `sources` that are not lists of str;
- `requires` naming a later or absent row;
- `options` that is not an object, or `first_session` that is not str/null.

**Check against code (`check`, run by the entry before any output).** It refuses unless all of these agree with the code:
- the same field list in the same order as the bound builder;
- for python rows, `builder` equal to the producing module;
- `point_in_time`, `requires` and `dtype` equal to the code's;
- `formula_sha256` equal to `formula_id` of the code's spec text.

**Entry.**
```
prepare_research_fields.py --registry <json> [--fields <a,b,...|all>] [--engine-exe <exe>] <builder argv>
```
- `--registry` binds every module named by a row, in row order, then runs `check`.
- `--fields all` expands to every row. With `--fields` absent, the builder's `DEFAULT_FIELDS` are used.
- Engine rows run through `prepare_research_fields_engine.engine_path(exe, rows)` only when `--engine-exe` is given.
- Without `--engine-exe`, engine rows are computed by Python, with a stderr notice.
- `--engine-exe` with no engine row requested raises `RegistryError`.
- With `--registry` absent, the builder is byte-identical to before.

## Evidence
All commands were run from `C:/atx-wt/pool-12` on the tree committed as `aa783bb1`. The `.gitattributes` file was added after these runs and has no effect on them.

```
$ "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tools
378 passed, 6 subtests passed in 174.19s (0:02:54)
exit_code=0
$ "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tests/fixtures/research_fields
9 passed, 1 skipped in 3.92s
exit_code=0
$ cd atx-engine/tools && "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_field_registry.py test_no_new_python_builder.py test_field_module_imports.py test_prepare_research_fields_reuse_keys.py test_prepare_research_fields_seal.py
30 passed in 10.13s
exit_code=0
```

Registry regeneration (scratch `gen_registry.py`: shims registered into the builder under the repository window, then `fr.dump(fr.generate(vars(b)))`):
```
92 rows 200282 bytes
6c56b739ac232b297d82987b60202d94b647206da0c37b2c51f36eb4e43e3e51
6c56b739ac232b297d82987b60202d94b647206da0c37b2c51f36eb4e43e3e51 *atx-engine/tools/field_registry.json
exit_code=0
```
`git show HEAD:atx-engine/tools/field_registry.json | sha256sum` gives `6c56b739...3e51` (the blob is LF).

Producer identity vs base (scratch `fp_diff.py`). It compares `producer_fingerprints` / `module_fingerprints` / `imported_code` of `git show d7c1c520:<file>` against HEAD sources, using HEAD's IMPORTS sets for both, so it isolates source edits. Result: 41 lines, all `SAME`. The lines are builder role/finra/th/lake/issuer/finra_sv; sec; price ×5; v8 ×3; holdings 13f/ftd/regsho/svx/xsw; v9 ×2 with imported_code; xdata ×3 with imported_code; gold; ohlc; ivshape; mgr13f with imported_code; divevent with imported_code; deals with imported_code; connected with imported_code. exit_code=0.

Base-vs-HEAD synthetic builds (scratch `ident_driver.py`, run with each tools tree on the `HoldFixture` + builder fixtures). The builds are full; holdings-only through the old carrier vs the new LATE module; reuse again; holdings-only reuse; partial reuse; plain. Compared by `ident_compare.py id_base/w id_head2/w`, exit_code=0:
```
again payloads EQUAL 52 manifest EQUAL(modulo documented keys) | head keys: ['runtime_versions'] reuse+: ['prior_runtime_versions', 'runtime_rule']
full payloads EQUAL 52 manifest EQUAL(modulo documented keys) | head keys: ['runtime_versions'] reuse+: []
only payloads EQUAL 2 manifest EQUAL(modulo documented keys) | head keys: ['runtime_versions'] reuse+: []
only-again payloads EQUAL 2 manifest EQUAL(modulo documented keys) | head keys: ['runtime_versions'] reuse+: ['prior_runtime_versions', 'runtime_rule']
partial payloads EQUAL 6 manifest EQUAL(modulo documented keys) | head keys: ['runtime_versions'] reuse+: ['prior_runtime_versions', 'runtime_rule']
plain payloads EQUAL 43 manifest EQUAL(modulo documented keys) | head keys: ['runtime_versions'] reuse+: []
role/bridge/events/ftd/regsho_threshold/security_master/short_volume_ext/thirteenf  payloads EQUAL, manifest EQUAL
```
"Documented keys" means these, and only these:
- code identity (`code_sha256*`, `producing_code_sha256_lf`);
- `runtime_versions`, `runtime_rule`, `prior_runtime_versions`;
- the prior `manifest_sha256`;
- path-hash `inputs_sha256`;
- `rows_available_on_or_after_2025_dropped` → `rows_sealed_dropped`.

HEAD reusing a prior that base code built (`ident_cross.py`, exit_code=0):
```
{"reused": 52, "computed": [], "not_reused": {}, "prior_runtime_versions": null, "payloads_equal": true}
```

**Diagnostics** (measurement, not a success claim). `cd atx-engine/tools && COLUMNS=600 python -m pytest -q -p no:cacheprovider --noconftest --tb=no -rfE .` gives `29 failed, 263 passed, 86 errors, 6 subtests passed`, exit_code=1. This is the input to ruling A1-C; the table is under "Legacy modules for T2".

## How root verifies
1. **Python.** Run the three pytest commands above, each exit 0. Cheap registry acceptance in production (repository window), run from `atx-engine/tools`:
   ```
   "C:/Program Files/Python312/python.exe" -c "import prepare_research_fields as b, field_registry as fr; d = fr.load(); fr.bind(vars(b), d); fr.check(vars(b), d); print('ok', len(d['fields']))"
   ```
   This prints `ok 92`.
2. **C++.** A1 changes no C++ and built none. `expected/manifest.normalized.json` now carries `rows_sealed_dropped`. Two consequences:
   - `atx-engine/tests/research/research_fields_fixture_test.cpp:170,197` (A2's file under P5) must read the same key.
   - Root runs `atx-engine-research-fields-tests --gtest_filter=ResearchFields*` only with A1 and A2 both merged. It is red between the two merges.
3. **Root identity run (fields v15 through the one entry).** v15 is the 84-field build of v8y-prereg §9 B, pinned in `scripts/specs/v8/waves/y-s.json`:
   - dir `build-equity/train-2020-2023-lo3-fields-v15`;
   - manifest sha256 `26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09`.

   v15 was built by builder blob `e8b57af5` (code_sha256_lf `74df97f9...`), which equals the d7c1c520 builder. Root takes its recorded v15 build-B argv and makes these substitutions:
   - drop the `import ...; y.register(vars(b)); b.main(...)` one-liner and invoke the script itself;
   - prepend `--registry atx-engine/tools/field_registry.json`;
   - keep `--fields` = v15's 84 names in v15's manifest order. Do not use `all`, which adds 8 non-v15 rows needing other inputs;
   - replace `--reuse <v15a dir> --reuse-sha256 <v15a pin>` with `--reuse build-equity/train-2020-2023-lo3-fields-v15 --reuse-sha256 26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09`;
   - set `--output build-equity/p9-a1-identity-fields-v15` (a new directory);
   - leave every other option (role, finra, price source, SEC / mgr13f / holdings stages and pins) unchanged.

   No `--engine-exe` is passed.
   ```
   "C:/Program Files/Python312/python.exe" atx-engine/tools/prepare_research_fields.py --registry atx-engine/tools/field_registry.json --fields <v15's 84 names> <v15 B argv, other options unchanged> --reuse build-equity/train-2020-2023-lo3-fields-v15 --reuse-sha256 26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09 --output build-equity/p9-a1-identity-fields-v15
   ```
   Expected:
   - `reuse.reused` = 82 and `reuse.computed` = [`nt_first_126`, `deal_pending`], both with reason "inputs differ". Their `imported_code` pins moved because the IMPORTS completion added the names listed under Files changed. All other fingerprints are SAME, per `fp_diff`.
   - The v15 manifest has no `runtime_versions`, so it is accepted as a legacy prior: `reuse.prior_runtime_versions` = null.
   - `seal.exclusive_end` = 2024-01-01, equal to v15's, so `load_prior` accepts it.
   - All 84 `<name>.f64` byte-identical to v15 (the sha256 values in the two manifests' `files`).
   - Manifest differences are confined to:
     - top-level `code_sha256*`;
     - `runtime_versions`;
     - the `reuse` block (from, manifest_sha256, lists, `runtime_rule`, `prior_runtime_versions`);
     - each reused entry's `reused_from` (dir = v15);
     - the 2 recomputed entries' producer identity and `imported_code`;
     - the `source_checks` key rename (`rows_available_on_or_after_2025_dropped` → `rows_sealed_dropped`) at 10 paths: deals/identity_bridge, issuer/{fund_events, identity_bridge, sic_events}, sec/identity_bridge, si_dtc, si_shares, v9/earn_season_rank/{fund_events, identity_bridge}, v9/nt_first_126/identity_bridge.

   Any payload mismatch is a stop.

## Deviations from brief
- **Ruling A1-C (PM): the conftest superseded-window bind stays through wave 1.** Removing it breaks 115 tests in 22 legacy-dated modules (table below), which go to wave-2 lane T2, together with G-P9's "bind removed" tick. Replacing the bind:
  - `test_prepare_research_fields_seal.py` pins A1's seal behaviour (refusal, rename, P13 log) in a fresh interpreter under the repository window.
  - The fixture generator asserts the repository window, and `expected/` was regenerated under it.

  Reported DONE per the ruling.
- **Rename at publish, not at the literal.** `rows_available_on_or_after_2025_dropped` is renamed to `rows_sealed_dropped` by `published_checks` over `manifest["source_checks"]` and `manifest["reuse"]` just before publish. The in-code literals (finra_field, load_bridge, load_events) are unchanged. Renaming them would have moved the finra and issuer producer fingerprints and the sec/v8/v9/deals host closures, which would recompute 30+ fields on every `--reuse` of v15. It would also have moved the role-builder golden hashes (rule 4).
- **IMPORTS completion** (v9_nt, deals). The FD-2 closure test found 7 undeclared cross-module names. Declaring them moves only the `imported_code` pins of `nt_first_126` and `deal_pending`; those 2 fields are recomputed once on the identity run.
- **The shims' `main` keeps the old behaviour**: it registers and calls `builder.main`, not `--registry`. Some SEC clock texts embed the seal date, so 17 rows' `formula_sha256` depend on the bound window (ea_* ×6, ins_* ×5, k8_* ×4, nt_first_126, deal_pending). A shim routed through `--registry` would refuse under the conftest window.
- **(6) `engine.py:13`**: moved to A2 (ruling P5).

## Cross-lane edits
None to another lane's files. Declared for A2:
- **The C++ fixture literal.** `research_fields_fixture_test.cpp:170,197` must read `rows_sealed_dropped`; A2 owns it under P5, and A2 was told. ResearchFields* is red between the A1 and A2 merges.
- **Reuse of engine output.** Python `--reuse` never reuses an engine-produced entry (P6, `engine_produced`), and the Python path never reads an engine row's `builder`.

Not changed (outside scope; two key names across manifest families):
- `prepare_recent_research.py:863`;
- the role-builder manifests (`test_linked_operating_v2/v3` still use the old key).

## Legacy modules for T2 (ruling A1-C; `--noconftest` at HEAD)
| module | tests | why it breaks without the 2025 bind |
|---|---|---|
| test_prepare_research_fields | 28 | Base role fixture has Oct-Dec 2024 sessions: `SealError: role contains a session at or after the research seal 2024-01-01` |
| test_prepare_research_fields_reuse | 15 | Same role SealError |
| test_prepare_research_fields_sec | 11 | Same role SealError |
| test_research_fields_holdings | 11 | Same role SealError (2024 sessions; stages year=2024) |
| test_prepare_research_fields_sv | 9 | Same role SealError (2024 CNMS) |
| test_prepare_research_fields_sic | 7 | Same role SealError |
| test_research_fields_connected | 7 | Same role SealError (2024 13F quarters) |
| test_research_fields_mgr13f | 6 | Same role SealError |
| test_prepare_research_fields_module_reuse | 5 | Same role SealError |
| test_lo1_delisting | 2 | Expected hashes / text embed "2025-01-01" (seal text in the pinned golden) |
| test_linked_operating_v3 | 2 | Pinned role-builder hashes move with the seal |
| test_linked_operating_v2 | 1 | Pinned role-builder hash moves with the seal |
| test_build_fundamental_events | 2 | 2024-dated events are sealed (`assert 2 == 0`) |
| test_prepare_identity_bridge | 1 | 2024-dated bridge row sealed (expected row list empty) |
| test_research_fields_gold | 1 | Per-source counts halve (`{'gold': 1, ...} != {'gold': 2, ...}`) |
| test_research_fields_v9_nt | 1 | Window-dated count (`2 != 1`) |
| test_research_fields_xdata | 1 | Window-dated count (`2 != 1`) |
| test_research_fields_deals | 1 | Knock-on only: passes alone; an errored module left shim fields bound in the builder |
| test_research_fields_divevent | 1 | Knock-on only (passes alone) |
| test_research_fields_ivshape | 1 | Knock-on only (passes alone) |
| test_research_fields_v8 | 1 | Knock-on only (passes alone) |
| test_research_fields_v8_quarters | 1 | Knock-on only (passes alone) |

Total: 115 tests in 22 modules. The PM's "25" was an estimate. T2 needs a PM ruling for the 3 golden-hash modules (rule 4 forbids editing expected hashes).

## Open risks
- **Window-dependent formulas.** The registry's `formula_sha256` is window-dependent for those 17 SEC-clock rows, because the clock text embeds the seal. A new window means regenerating the registry; `check` refuses a stale one before any output.
- **`rows_on_or_after_2025_skipped`** (th group, `prepare_research_fields.py` ~1056/1063) is the same mislabel. It is not renamed (not in brief).
- **dtype declared by rule.** `ea_time_of_day` is the only row whose declared dtype (group) differs from the old `grp_` prefix inference. A2's VM must take dtype from the registry.
- **`sources` is informational.** Nothing checks it against a run's argv.
- **Other Python fields-manifest readers** do not yet call `require_research_seal`: `prepare_recent_research --check-fields` and the atx-impl tools. The function is ready for them.
- **The engine path** stays opt-in (`--engine-exe`) until root's TRAIN identity run.

## Ledger candidates
- **Rename keys at publish time.** Renaming a manifest key inside producer code moves its `code_fingerprint` closure and recomputes every dependent field on `--reuse`. Rename at publish time instead (`published_checks`); P9 A1 measured 30+ fields at risk.
- **SEC clock texts embed the research seal.** As a result, `formula_id` and `field_registry.json` for 17 rows depend on the bound window. Regenerate and test the registry only in a fresh interpreter under the repository window.
- **`autocrlf=true` rewrites LF files on checkout.** Any byte-pinned new file needs a `-text` attribute; `atx-engine/tools/.gitattributes` does this for the registry.

## Fix round 1
**Outcome:** DONE. FIX_BASE `88c9efa2`. The review's one major (M1) is fixed. m3 was adopted under PM ruling A1-M3; the other minors stay deferred, as the PM ruled.

**Commits** (branch `feat/p9-a1-20261003`, pool 12):
- `d672eb8d` fix(fields): registry check/regenerate accept engine-only rows (P9 A1 fix 1). Changes `atx-engine/tools/field_registry.py`.
- `c43ff27e` test(fields): engine-only rows pass check and the round-trip; today's registry regenerates byte-identically (P9 A1 fix 1). Changes `atx-engine/tools/test_field_registry.py`.

**Finding addressed: M1.** Before this fix, check and generate refused `kind: engine` rows that have no Python producer. That blocked DEC-5 appends (`field_registry.py` `check` / `generate`; `test_field_registry.py:86`, `:91-101`).
- `check` now compares only the Python-producible rows with the code: their set and relative order, plus `builder` (python rows), `point_in_time`, `requires`, `dtype` and `formula_sha256`, as before. A python row the code cannot produce is still refused. An engine-only row (an engine row whose name the bound builder cannot produce) is held by `validate` alone.
- New `engine_only(ns, doc)` returns the names of those rows in registry order.
- New `regenerate(ns, doc)` is the generator round-trip:
  - It calls `generate` with `doc`'s engine twins flipped, and places those rows at `doc`'s Python-producible positions in code order.
  - It keeps `doc`'s engine-only rows verbatim at their positions. It drops a python row the code cannot produce, and appends any code field that has no row.
  - The top-level keys come from `generate`.
  - With no engine-only rows, it returns `generate(ns, engine_flips(doc))` exactly.
  - `generate` itself is unchanged. It still flips only rows the code produces, and still refuses other names ("not producible"). Root's A2 flip of si_shares, si_dtc and vol_126 through `generate` is unaffected.
- The `entry` refuses a requested engine-only row before any output, because Python has no fallback for it:
  - without `--engine-exe`: "engine-only rows X have no Python producer; they need --engine-exe ...". This includes `--fields all`.
  - with `--engine-exe` when `prepare_research_fields_engine.ENGINE_FIELDS` does not route the row: "... have no route in prepare_research_fields_engine ...". Today the builder would otherwise refuse it opaquely in `run()`.

  Engine twins behave exactly as before.
- Tests:
  - The committed-file test (fresh interpreter, repository window) now asserts three things: `dump(regenerate(file)) == file bytes`; `dump(generate(engine_flips(producible rows))) == dump(producible rows)`, which is the flag-absent identity; and 92 producible rows.
  - `test_valid_and_in_v15_order` counts the 92 rows and checks that holdings come last over the producible rows only. The `engine_flips ⊆ ENGINE_FIELDS` pin is relaxed: an `ENGINE_FIELDS` row that is flipped must carry its own name as kind id (A2's ids). Any other engine row may carry any id.
  - New `test_engine_only_rows_appended_to_the_committed_file_check_and_round_trip` (fresh interpreter): the committed file plus two synthetic engine-only rows (row 40 and the end) passes `bind` + `check` and round-trips byte for byte. `generate` alone refuses them.
  - New `Validation.test_engine_only_rows_pass_check_and_the_generator_round_trip`: an engine-only row next to an A2 twin flip passes `check` and is kept in place by `regenerate`. Each of these is still refused by `check`, and `regenerate` does not reproduce it: a python-declared extra row, producible rows swapped around the engine-only row, and a dropped code row.
  - New `Entry.test_engine_only_rows_have_no_python_fallback`: the entry refuses an engine-only row in three cases (without an exe, under `--fields all`, and with an unroutable `--engine-exe`) and writes no output dir in any of them. The registry's other rows build the plain builder's bytes.

**Identity.** `field_registry.json` is not modified (its sha256 is still `6c56b739...3e51`). For today's rows, `generate` and `regenerate` both reproduce its bytes; the fresh-interpreter test pins this. Python-producer rows and the builder are untouched. No producer fingerprint moves, so the root v15 identity run in "How root verifies" is unchanged.

**Evidence** (pool-12, `PYTHONDONTWRITEBYTECODE=1`; tools and fixtures run as separate processes, per m5):
```
$ "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tools
381 passed, 6 subtests passed in 159.43s (0:02:39)
exit_code=0
$ "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tests/fixtures/research_fields
9 passed, 1 skipped in 4.54s
exit_code=0
$ cd atx-engine/tools && "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_field_registry.py test_no_new_python_builder.py test_field_module_imports.py test_prepare_research_fields_reuse_keys.py test_prepare_research_fields_seal.py
33 passed in 11.83s
exit_code=0
```
There are 3 new tests (378 → 381 in tools; 30 → 33 in the focused set).

**Minors.** m1, m2 and m4–m7 stay deferred (PM); m3 was adopted (below). The killed implementer's partial edits for m1 (refuse every unroutable engine row behind `--engine-exe`) and m7 (strict `first_session` date) were removed from `field_registry.py`. The `--engine-exe` routing check now covers engine-only rows only, because that is part of M1. Engine twins keep m1's deferred behaviour.

**m3 adopted (PM ruling A1-M3), commit `4df3f7b7`.** `require_research_seal` now refuses a `seal` that is present but not an object (a string, null or a list) instead of logging it as legacy. A seal object takes the same path as before, whether its `exclusive_end` is equal, different or missing. An absent seal is still logged and accepted (P13). The fields builder has written `seal` as an object since `c615ce36`. A new window-independent test covers this: `test_prepare_research_fields_seal.py::MalformedSealBlock`. Tools rerun: `382 passed, 6 subtests passed in 171.56s`, exit 0.

**How root verifies (delta).** Run the pytest commands above as separate processes. The cheap production acceptance command from "How root verifies" still prints `ok 92`. Once a DEC-5 row is appended, it prints the total row count.

## Fix round 2
**Outcome:** DONE. FIX_BASE `e5780efb`; the re-review (`task-A1-rereview-1.md`) blocked there. This round changes tests only: `field_registry.py`, `field_registry.json` (sha256 still `6c56b739...3e51`) and the builder are unchanged since `e5780efb`.

**Commit:** `8fd511d0` test(fields): registry tests hold with engine-only rows (P9 A1 fix 2). Changes `atx-engine/tools/test_field_registry.py`.

**N1 (major; closes M1).** The `CommittedRegistry` tests no longer assume the committed file has no engine-only rows. They hold only the Python-producible rows to the code. A new helper, `engine_only_names(doc)`, runs `fr.engine_only` with every shim bound. Per test:
- `test_this_harness_generates...` compares through `fr.regenerate`, which keeps engine-only rows verbatim.
- `test_lane_a2_flip...` pins A2's three ids among the file's twins, which are the engine flips that are not engine-only. It calls `generate` with those twins only and compares the result with the producible rows.
- `test_dtype_is_declared...` computes the `group` and `grp_` sets over the producible rows.
- `test_engine_only_rows_appended...` finds its two synthetic names in order inside `only`, and checks that each one appears in the refusal text.
- `test_generated_from_the_four_mechanisms...` passes the file that the harness's `fr.load()` reads (`fr.DEFAULT_PATH`) to its fresh interpreter, so a patched path reaches the subprocess too.

Regression: two new classes, `CommittedRegistryWithADec5Row` (f64) and `CommittedRegistryWithADec5GroupRow` (`dtype: group`). Each patches `fr.DEFAULT_PATH` to a copy of the committed file with one appended engine-only row, `syn_dec5`, and reruns every inherited `CommittedRegistry` test unedited. Each also adds `test_the_copy_holds_the_engine_only_row`. All 16 pass. A DEC-5 append therefore needs no edit to A1's tests.

**N2 (minor; same lines).** `test_valid_and_in_v15_order` asserts again that every engine twin is routable: twins ⊆ `prepare_research_fields_engine.ENGINE_FIELDS`, and each twin's kind id is its own name. Engine-only rows are exempt.

**Evidence** (pool-12, `PYTHONDONTWRITEBYTECODE=1`):
```
$ cd atx-engine/tools && "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_field_registry.py
35 passed in 5.22s
$ "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tools
398 passed, 6 subtests passed in 163.86s (0:02:43)
exit_code=0
```
The tools count went from 382 to 398: the two regression classes add 16 tests.
