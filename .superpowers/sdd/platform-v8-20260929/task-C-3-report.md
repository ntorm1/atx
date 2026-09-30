# Task C-3 report: --reuse for the SEC and holdings field modules

Lane C, branch `feat/platform-v8-c-20260929` (pool-9). Status: DONE_WITH_CONCERNS (see "Open risks").

## What was built

- `atx-engine/tools/code_fingerprint.py`: `Host(source, handles, orchestration)` and `host_names(statements, handles)`.
  `fingerprints(..., host=)` adds a `host_closure` to the hashed document: the builder closure of every name the
  module group reads through its handle (`h.X`, `self.h.X`, `ns["X"]`, `ctx.ns["X"]`). Without `host`, the output is
  unchanged. The builder's six group fingerprints before and after C-3: role, finra, th, lake and issuer are
  identical. finra_sv changed (see below).
- Module interface. Each module exports `PRODUCERS = {group: (entry names,)}`, `HOST_HANDLES`,
  `producer_group(name)`, `field_spec(name)`, `reuse_inputs(name, options)` and `entry_inputs(entry)`.
  - `research_fields_sec.py`: one group, `sec: ("SecFieldModule",)`, read through `h`. Computed SEC entries now
    record `producer` (module name plus code identity), as the holdings entries already did. The source check is the
    three stage manifest SHA-256s of the field plus the SEC identity bridge SHA-256.
  - `research_fields_holdings.py`: `13f: build_13f`, `ftd: build_ftd`, `regsho: build_regsho`, `svx: build_svx`,
    read through `ns`. The source check is the kind's stage manifest SHA-256s (`stage_manifest_sha256`, already
    recorded).
- Builder (`prepare_research_fields.py`):
  - `load_prior` was extracted from `reuse_fields`, unchanged.
  - `producer_source(ident, current, what)` is generalised to a module source.
  - New: `module_source`, `module_code_identity`, `module_fingerprints` and `module_formula`.
  - New: `reuse_module_fields(module, ...)` implements `REUSE_MODULE_RULE`. A field is copied only when all of these
    hold:
    - same layout and pin;
    - same formula keys (formula_id, units, clock, staleness, source_columns, definition, domain);
    - same group fingerprint, computed for the module and builder code that produced the prior payload (the entry's
      `producer`; the builder named by the manifest, or by `reused_from.host_code` through a chain). The code is found
      as the current code or its git blob;
    - same stage pins;
    - every required field is reused.
  - The copy is re-hashed against the prior pin. `reused_from` records producer, host_code, producer_sha256,
    producer_code, inputs, payload_sha256 and mode.
  - New: `merge_module_reuse` merges the module's decisions into the manifest's reuse block, in registry order. It
    carries the prior source check of a group only when every field of the group is reused, and records the prior
    check as partial otherwise.
  - `run()`: the builder's own reuse now skips the module fields. Each FIELD_MODULES module (SEC) is decided by
    `reuse_module_fields`, and `compute` receives only the non-reused names.
- Holdings wrapper:
  - The publish hook calls `reuse_module_fields` / `merge_module_reuse` when `--reuse` is given. It uses
    `group="holdings.<kind check key>"`.
  - `build_all(..., reused, prior_checks)` computes only the non-reused fields and appends the reused entries in
    registry order. A kind whose fields are all reused carries its prior check into `source_checks.holdings`, with
    keys in kind order.
  - `code` is `module_code_identity(module)`, which equals the old `code_identity(Path(__file__))` value for value.
  - In the carrier case, `mkt_ret` is also removed from the reuse block.
- The sv_ratio126 directory source. This is needed for "63 reused": the fields-v9 recipe includes sv_ratio126, which
  the R1 rule never reused ("a source without a file SHA-256").
  - `sv_window_files(listing, role_days)` was extracted from `sv_field` without changing its behaviour.
  - `REUSE_DIRECTORY_RULE`: the CNMS directory source passes when the files this run reads for this role
    (`sv_window_files` of the current listing, the same function `sv_field` uses) re-hash by `SV_FILES_LIST_RULE` to
    the recorded `files_sha256`, `files_read`, `first_file_date` and `last_file_date`.
  - The reuse block gains `directory_rule` only when such a source was checked. The extraction changes the finra_sv
    producer fingerprint once (c0064df7… vs f5b29a89…).

Without `--reuse`, every payload byte is unchanged. The only manifest change is the new `producer` key on SEC field
entries (holdings entries already had it). `ByteIdentity` (SEC and holdings) and all builder tests pass.

Tests (synthetic, `atx-engine/tools/test_prepare_research_fields_module_reuse.py`):

- `test_reuse_self_reports_63_reused`: the 63-field recipe is DEFAULT 8 + ISSUER 32 + sv_ratio126 + 14 SEC + 8
  holdings. Rebuilding with `--reuse` of itself gives reused 63, computed [], and not_reused {}. The files map and
  every payload are identical, and each entry equals the prior one minus `reused_from`. The sec and per-kind holdings
  source checks are the prior's. A reuse of that reuse still gives 63.
- `test_one_producer_edit_recomputes_one_field`: an AST edit to `build_ftd` is simulated through `module_source`, with
  git holding the unedited blob. Only `ftd_shares_ratio21` is computed, with reason "producing code differs
  (research_fields_holdings.py group ftd …)". It has the same bytes, and its producer is the edited code. The other
  62 are reused, with the other holdings fields found via git blob.
- `test_stage_pin_change_recomputes_its_kind`.
- `test_sv_directory_reuse_and_change`: sv_ratio126 is reused. After a CNMS file in the read set is re-gzipped
  (same rows, other bytes), it is recomputed. A reuse run that checks no directory source has neither
  `directory_rule` nor `module_rule`.
- `test_module_groups_follow_their_own_closure`: a comment does not count. A `build_ftd` edit changes only the ftd
  group. A builder `load_bridge` edit changes SEC only, a `FieldWriter` edit changes both modules, and a
  `reuse_fields` edit changes neither.
- `test_prepare_research_fields_sec.py` ByteIdentity: "--reuse never copies a SEC field" became "a self reuse copies
  all of SEC_RUN; the sec source check is the prior's".

Run (`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider`):
test_prepare_research_fields*.py + test_research_fields_holdings.py + test_record_store.py gave 105 passed.
test_linked_operating_v3.py gave 8 passed.

## How root verifies (TRAIN fields only; after the W0E merge)

Run these after W0E is merged. W0E edits the builder `SEAL_*` constants, which the SEC closure reads through `h`, and
the holdings `SEAL` constant. Any build made before that merge recomputes the SEC and holdings fields once.

Let F = `build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9` (63 fields, role through 2022-12-30). Take its
argv from its run receipt / prepare_recent_research log.

1. Rerun that argv with `--reuse F --reuse-sha256 <sha256 of F/manifest.json> --output
   build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9-c3a`. Expected: the 40 builder fields are reused.
   These are recomputed:
   - sv_ratio126 (finra_sv producer changed once);
   - the 14 SEC fields (F's SEC entries record no producer: "producing code not recoverable");
   - the holdings fields, unless the builder blob and holdings blob `8ba94e78…` named by F are in git and their
     closures are unchanged. `reuse.not_reused` states each reason.
2. Rerun the same argv with `--reuse <c3a> --output ...-fields-v9-c3b`. Expected: `reused 63, computed 0` (the
   brief's count). The run is dominated by copy plus hash (about 10 s estimated, from 59-121 s).
3. Identity:
   ```
   "C:/Program Files/Python312/python.exe" -c "import json,sys; m=[json.load(open(p+'/manifest.json')) for p in sys.argv[1:]]; print('files equal:', all(x['files']==m[0]['files'] for x in m)); drop=lambda e:{k:v for k,v in e.items() if k not in ('reused_from','producer')}; print('entries equal:', all([drop(e) for e in x['fields']]==[drop(e) for e in m[0]['fields']] for x in m)); b=m[-1]['reuse']; print('reused', len(b['reused']), 'computed', b['computed'], 'not_reused', b['not_reused'])" build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9 build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9-c3a build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9-c3b
   ```
   Expected: `files equal: True`, `entries equal: True`, `reused 63 computed [] not_reused {}`.
4. A one-field change (optional): edit `build_ftd` in a scratch branch, rebuild with `--reuse <c3b>`, and expect
   only `ftd_shares_ratio21` computed.

## Deviations from the brief

- "63 reused" holds for a directory built by C-3 code (and after W0E), not on the first rebuild from fields-v9. F's
  SEC entries carry no producer identity, and no sound fallback exists: the builder blob does not determine the
  module version.
- The sv_ratio126 directory rule (`REUSE_DIRECTORY_RULE`) and the `sv_window_files` extraction were added to the
  builder. They were needed for 63 of 63, and the brief lists the source-check lines 2917-2923. The extraction
  changes the finra_sv producer fingerprint once.
- SEC reuse granularity is the whole `SecFieldModule` class, because its 14 fields share one streaming pass: any SEC
  code edit recomputes all 14. Holdings is per kind (13f: 5 fields; ftd, regsho and svx: 1 each).
- The holdings `source_checks.holdings.stages` block is this run's check (`files_read` counts only the files read),
  not the prior's.

## Cross-lane edits

None. W0E's lines are untouched: the builder and holdings seal constants, and SEC line 76.

## Open risks

- The W0E seal constants are in both module closures (builder `SEAL_NS` via `h`, holdings `SEAL`). Every
  pre-merge build therefore recomputes the SEC and holdings fields on its first post-merge reuse. This is correct
  behaviour, but it affects the count on step 1.
- The existing SEC and holdings synthetic fixtures use 2024 sessions (roles 2024-05..12 and 2024-10..12). After W0-1
  seals 2024, W0E / root may need to redate them; the new C-3 test reuses those fixtures.
- Hidden-data slip: while checking the fields-v9 field count, a glob over `build-equity/*fields*/manifest.json` also
  opened the six `recent-fast-validation-2023-2024-v1-fields-v1..v6/manifest.json` files. Only these were printed:
  field count, whether sv_ratio126 is listed, `role.last_session` and whether a reuse block exists. No coverage or
  other statistic was printed or used.
