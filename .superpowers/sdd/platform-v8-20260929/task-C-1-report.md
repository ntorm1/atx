# Task C-1 report: fit and card stores keyed by role and window

Lane C, branch `feat/platform-v8-c-20260929` (pool-9). Status: DONE.

## What was built

Generic machinery (atx-engine/tools, used by the builder, the fitter and the card):

- `atx-engine/tools/code_fingerprint.py` (new). The AST-closure fingerprint, moved out of `prepare_research_fields.py`
  unchanged: `fingerprints(source: bytes, producers: dict, orchestration=frozenset()) -> {group: sha256 | None}`,
  `fingerprint(source, entries: tuple) -> str`, `Module` (bindings, `reach`, `closure`), `bound_names`, `free_names`,
  `code_dump`. Comments, docstrings and formatting do not count; any code, constant or import a producer reaches does.
- `atx-engine/tools/record_store.py` (new). `RecordStore(root).get(kind, key) -> body | None`,
  `.put(kind, key, body) -> bool`. File `ROOT/<kind>/<sha256(canonical {kind, key})>.json` holds
  `{schema: atx.record-store/v1, kind, key, body, content_sha256}`. A read is a hit only if schema, kind, the whole key
  object and the content SHA match. Write = temp sibling, fsync, `os.replace`. The body keeps its key order; the
  content SHA is order-insensitive.
- `prepare_research_fields.py`: `producer_fingerprints` now calls `code_fingerprint.fingerprints(source,
  FIELD_PRODUCERS, PRODUCER_ORCHESTRATION)`; `_bound_names/_free_names/_code_dump/_SCOPES` and the `ast`/`copy`
  imports are removed (move only). Checked: all six group fingerprints are equal before and after the move on this
  source and on the HEAD source (so fields-v9's builder fields stay reusable).

Fitter (`atx-impl/tools/fit_composition_weights.py`):

- `producer_fingerprint(funcs: tuple) -> str` (lru-cached): `code_fingerprint.fingerprint` over this file.
  `FACTOR_PRODUCERS = ("factor_record", "Context", "PricePanel", "neutralization_basis", "centered_tied_ranks")`,
  `CONTEXT_PRODUCERS = ("Context",)`, `AIM_PRODUCERS = ("aim_record", "Context")`.
- `window_id() -> str`: `from engine_tools import research_window; research_window.WINDOW_ID` (the W0E helper
  module and name, confirmed by lane W0E); falls back to the literal `"research-window-v2"` only on ImportError, with a
  comment that root removes the fallback after the W0-1 merge (constant `WINDOW_ID_FALLBACK`).
- `WorkStore(root, role, window=None)`: root `W/<role sha16>-<window id>/`; `context/<CONTEXT_PRODUCERS fp>/` holds the
  price-risk context; record kinds `factor` and `aim`. Key = `{schema, semantics_tag, role_manifest_sha256 (full),
  window_id, cache_payload_sha256, producer_fingerprint}` (+ `aim_tag`, `train_window_ns` for aim). The screen, the
  VM identity, the DSL text and the field pins are no longer in the key: a factor record is a function of the context
  and the signal bytes only, so the same signal is a hit in any library. Records verify key + content SHA + shape.
- `factor_record(context, signal)` and `aim_record(context, signal, train_mask)` return bodies without per-candidate
  metadata; the old `work_key_text/work_key_sha256` are removed.
- `Context` meta no longer contains `script_sha256`: the digest is a pure content digest (arrays, role, window,
  refused decisions). The code that built a stored context is bound by its directory name.
- `stored_factor_series(work, role_sha, payloads, context_sha=None) -> {id: np.ndarray}` for readers of the store.
- Themes: `prior_themes() -> (themes, source)` reads `atx-impl/strategies/alphas/registry.json` (`atx.alpha-registry/v1`,
  `themes{name: text}`) when the file exists, else the in-file `V4_THEMES + V7_APPENDED_THEMES`.
  `themes_preregistered` = `V4_THEMES` + the registry's non-v4 themes that some candidate declares, in registry order
  (identical bytes to today when the registry lists ownership_flow as its only non-v4 theme). A malformed registry
  refuses (exit 1).
- `SCRIPT_SHA256` stays in the outputs as provenance only.

Card (`atx-impl/tools/alpha_report_card.py`):

- New module-level producers `rank_signal`, `invariant_block` (size / FF12 splits, daily pnl / turnover / coverage),
  `decay_block` (decay curve + cumulative h 5/21/63 series for runner_check), `block_json` / `block_arrays`,
  `size_field_name`. `CARD_PRODUCERS` = those plus `row_decay`, `Geometry`, `size_groups`, `ff12_groups`, `GroupLabels`.
- `CardStore(root, role, min_names, fields)`: kind `card` under the same root as the fitter
  (`W/<role16>-<window>/card/`). Key = `{schema atx.alpha-report-card-invariant/v1, role (full), window_id,
  producer_fingerprint, min_names, fields: {me_company: sha|None, grp_ff12: sha|None}, payload_sha256}`.
- `build`: every candidate's signal is still loaded and ranked (the correlation block needs all ranks); invariant
  blocks are read from the store, and only the missing ones run size/FF12 ICs, the book and `row_decay` (on the
  missing candidates' ranks only). Group label ranks are skipped when every block is a hit. A computed block is
  normalised through its stored form, so a hit and a recompute feed the page builder identical values.
- CLI: `--work-dir W` (store base, same value as the fitter's) and `--coverage-flags` (report only: `coverage_flag` in
  each card = years with mean coverage < 0.8 and years with no finite signal; `coverage_low` in the index rows; the
  candidate is always carded and ranked). stderr reports `card: computed K, reused M`.

Tests (all synthetic):

- `atx-impl/tools/test_fit_composition_weights_store.py`: `test_fit_store_reuses_across_libraries` (44-member library
  then the 48-member library on one store: `fit: computed 44, reused 0` then `fit: computed 4, reused 44`; files
  byte-identical to a store-less fit; deleting the store gives the same bytes), `test_comment_edit_keeps_store` (edited
  module copies: comment + docstring edit and an orchestration edit reuse 48; a value-neutral producer edit recomputes
  48; admission bytes equal apart from the script SHA), `test_fit_store_keyed_by_role_and_window` (two roles and two
  windows never share; a foreign record at the right path is a miss), `test_stored_factor_series_serves_the_monitor`,
  `test_themes_come_from_the_registry_when_present`.
- `atx-impl/tools/test_alpha_report_card_store.py`: `test_card_invariant_block_reused` (3-member then 5-member library:
  `card: computed 3, reused 0`, then `computed 2, reused 3` with bytes identical to a store-less run; a different
  admitted set reuses 5 and equals a fresh run, only the correlation block differs), `test_a_tampered_or_foreign_block_
  is_a_miss`, `test_cards_byte_identical_to_the_pre_store_card` (git blob 75f6e159 = the card at ef11f462),
  `test_card_low_coverage_reported`, `test_without_the_switch_nothing_changes`.
- `atx-engine/tools/test_record_store.py` (3 tests).
- `test_fit_composition_weights.py` adapted to the new layout: store paths, record body nesting, `work_shape` now
  normalises the pre-W3, W3 and v8 layouts; `CacheInvalidation` and `WorkStoreKeys` assert the new contract (script
  SHA is not a key, producer fingerprint is; records follow signal bytes, not ids, pins, DSL text or screen); a
  module-level fixture keeps the fitter tests on the in-file theme list (hermetic once the registry lands).

Test run: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider` over
test_fit_composition_weights.py, test_fit_composition_weights_store.py, test_alpha_report_card.py,
test_alpha_report_card_store.py, test_book_monitor.py, test_mega_report_pitch3.py, test_mega_report_sig_corr.py:
175 passed. atx-engine/tools test_record_store.py + test_prepare_research_fields_reuse.py: 18 passed.

## How root verifies

Byte identity cannot include `inputs.script_sha256` (the file changed); `inputs.context_sha256` also changes once,
because the digest no longer binds the script SHA (it is now stable across later fitter edits). This is the W3
precedent ("equal after dropping inputs.script_sha256, context_sha256").

1. Fit identity and the reuse count. Take the argv of `build-equity/mega-weights-v71-ew-run1` (its receipt), change
   `--work-dir` to `build-equity/fit-work` and `--output` to `build-equity/mega-weights-v71-ew-c1a`; run it through
   `scripts/run_bounded_research.py`; then the same with output `-c1b`. Expect stderr `fit: computed 48, reused 0`,
   then `fit: computed 0, reused 48`. For the brief's count: first the v7.0 fit argv (mega-weights-v70-ew receipt) with
   `--work-dir build-equity/fit-work --output build-equity/mega-weights-v70-ew-c1`, then the v7.1 argv with
   `--output build-equity/mega-weights-v71-ew-c1c`: expect `fit: computed 4, reused 44`.
2. Compare each output with the accepted one:
   ```
   "C:/Program Files/Python312/python.exe" -c "import json,sys; a,b=[json.load(open(p+'/admission.json')) for p in sys.argv[1:3]]; [d['inputs'].pop(k) for d in (a,b) for k in ('script_sha256','context_sha256')]; print('admission equal:', a==b); print('csv equal:', open(sys.argv[1]+'/admission.csv','rb').read()==open(sys.argv[2]+'/admission.csv','rb').read()); w,v=[json.load(open(p+'/composition_weights.json')) for p in sys.argv[1:3]]; [d['provenance'].pop(k) for d in (w,v) for k in ('script_sha256','context_sha256','admission_sha256')]; print('weights equal:', w==v)" build-equity/mega-weights-v71-ew build-equity/mega-weights-v71-ew-c1a
   ```
   (repeat for `-c1b` and `-c1c`). Expected: all three `True`. `admission.csv` is byte-identical.
3. Cards. Take the argv of the `mega-cards-v71` run and keep `--admission build-equity/mega-weights-v71-ew/admission.json`
   (the accepted file: the index lists its path and SHA); add `--work-dir build-equity/fit-work`, output
   `build-equity/mega-cards-v71-c1a`, run twice (second output `-c1b`). Expect `card: computed 48, reused 0`, then
   `card: computed 0, reused 48`; `diff -r build-equity/mega-cards-v71 build-equity/mega-cards-v71-c1a` and `-c1b`
   empty (every card, page, index, daily_sleeve.csv and manifest.json byte-identical).
4. Spec wiring (lane A / root): `fit.work_dir: "build-equity/fit-work"` in every v8 spec (one store for all
   libraries); `card.flags: ["--work-dir", "build-equity/fit-work"]` (the card step passes `card.flags` through);
   the monitor step already passes `--fit-work <fit.work_dir>`, and book_monitor reads the v8 store.

## Deviations from the brief

- Store base: the fitter keeps `--work-dir W`, and the card gains `--work-dir W`; the store root is
  `W/<role_sha16>-<window_id>/` (W = `build-equity/fit-work` in the specs). The tools never hard-code `build-equity`.
- The factor record key is (signal payload SHA, producer fingerprint) plus role, window and semantics tag, as briefed;
  this deliberately drops the W3 key parts screen, VM identity, DSL and field pins (they do not enter the record).
- The context is keyed by its own fingerprint (`CONTEXT_PRODUCERS`), and its digest no longer binds the script SHA.
- Coverage flag (test `test_card_low_coverage_reported`, review focus 6) is behind `--coverage-flags`: adding it
  unconditionally would break the card byte identity. Suggest root turns it on in v8 specs.
- `test_comment_edit_keeps_store` uses an edited copy of the module (not a git edit): the fingerprint is computed from
  the loaded file.

## Cross-lane edits

- `atx-impl/tools/book_monitor.py` `fit_records` (unowned file, 4 lines): reads the v8 store first through
  `fcw.stored_factor_series(work, role, want, admission.inputs.context_sha256)`, then the legacy layouts as before.
  Without it the M4 crowding check would silently find no records after C-1. test_book_monitor.py passes unchanged.
- `atx-engine/tools/prepare_research_fields.py` (lane C's own file, task C-3): move-only extraction described above.

## Open risks

- A partial card run computes `row_decay` for the missing candidates only (smaller GEMM height). Per-row results are
  independent of the batch in the tests on this machine (the `computed 2, reused 3` case is byte-identical to a full
  run), but root should confirm with the v7.0 then v7.1 card sequence (step 3 with a v7.0 card first).
- `window_id()` falls back to the literal until W0-1 is merged; after the merge `engine_tools.research_window.WINDOW_ID`
  is read. Root: delete the fallback branch and `WINDOW_ID_FALLBACK`.
- W0-1 replaces `FIT_BEGIN_NS`/`TRAIN_END_NS` with reads from research_window: those statements are in the producer
  closures (through `RoleManifest`), so the first fit after the W0-1 merge recomputes every record once. Expected.
- The store is never garbage-collected: records of old producer versions stay until the directory is deleted
  (factor record about 20 KB, card block about 120 KB per candidate on a 3-year role). Deleting it is always safe.
- The registry is read unpinned for the admissible theme list and the order of appended themes only; the theme of
  each candidate stays pinned by the library / recipe.
