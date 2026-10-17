# Lane A4 report (stretch; P9 RESUME 9)

## Outcome
@@OUTCOME@@

## Branch / SHA
`feat/p9-a4-20261005` in `C:/atx-wt/pool-27`. Pre-flip head (complete and tested): `@@PREFLIP@@`. Flip commits (each
its own, at the end; root merges them only after its TRAIN identity per field): `@@FLIPS@@`. Final head `@@HEAD@@`.
Root merged: `685e238b` (`99ae81e2`; no conflict). T2 `dfa4218d` merged earlier (`ebfb333e`).

## History of the lane
| commit | what |
|---|---|
| `1e4904b7` | (R8 agent) A1-SHIM, S-R2, N-1 (Python) |
| `5c53c2c9` | DIRTY-2: the R8 agent's 25 uncommitted paths as they stood (untested as committed) |
| `4be4f3f3` | handoff "R8 slice, reconstructed" + the R8 records (`a4-configure-dev.log`, `a4-evidence/gtest-p9-a4-a-whole.log`) |
| `99ae81e2` | merge of root `685e238b` |
| `984c4a77` | code slice: one short-side reader (streamed, bounded), kernels, A3-3 count, registry owner suffix |
| `ee19dc4a` | tests slice: kernel / batch-unit gtests, engine-route pytest, mechanical pins |
| `@@FLIPS@@` | the two `kind: engine` flips |

## Required items -> change -> test -> evidence

| item | change (file:line at the pre-flip head) | test | evidence |
|---|---|---|---|
| Clock primitives (Calendar / Latest / Windowed) | `fields/session_clock.hpp` (`SessionCalendar`, `LatestRows`, `WindowedSums<T>`), `session_clock.cpp`; `asof_series.hpp` checked first (header :22-25: another rule) | `ResearchFieldsA4Clock.*` (6) | @@GT@@ |
| FTD / RegSHO stage readers, AL-SIG style, ONE reader interface (owner's bar) | `sources/short_side.hpp` `read_short_side<Rows>` / `read_short_side_all<Rows>` over `PinnedStage::for_each_group` (one row group at a time); per-Rows column layouts with compile-time checks in `sources/short_side.cpp`; `parquet_columns.hpp` `code_column` / `flag_column` (text -> code, no per-row string) | fixture identity gtests below read every stage file through it | @@GT@@ |
| Builder kinds + registry rows | kinds `ftd_shares_ratio21`, `regsho_threshold_days63` (`registry.cpp` kind table, 13 rows); specs `holdings_specs.cpp` (registry spec_text, fingerprint-pinned); registry rows (owner suffix in `984c4a77`, flips at the end) | `ResearchFieldsRegistry.KindLookup`, `ResearchFieldsHoldingsFixture.FormulaFingerprintsAreTheRegistrys`, `.RegistryBuildEntriesAreThePythonEntries`, `.MissingOrWrongInputsAreRefused` | @@GT@@ |
| Bounded memory, no per-row allocation in the hot loop | FTD: `FtdIndexer` keeps one entry per settlement date (sorted vector, last-slot fast path) and the role lines' fails only; `FtdKernel` keeps kFtdWindow + 1 prefix rows instead of the (dates + 1) x lines matrix. Reg SHO: `RegshoIndexer` holds one file's usable rows; `ListingIndexer` keeps role rows only; `RegshoKernel` scratch sized once, window summed over contiguous rows. Row loops allocate nothing (output row and scratch reused) | `ResearchFieldsHoldingsKernel.FtdRingIsTheFullMatrixInAnyOrder`, `.RegshoLateRuleIsPerFileAcrossBatches` | @@GT@@ |
| Planted-leak probe per builder, with teeth | `ResearchFieldsHoldingsProbe.FtdLookAheadProbeHasTeeth`, `.RegshoLookAheadProbeHasTeeth` (intrinsic teeth: the kernel one session late is caught) | same | @@GT@@ |
| Fixture byte identity vs the Python builders | `tests/fixtures/research_fields_holdings/` (T2's re-dated synthetic stages; expected = the EXISTING Python builder's payloads + normalized manifest) | pytest `test_holdings_fixture.py` (2: the generator reproduces every committed byte); gtests `ResearchFieldsHoldingsFixture.FtdIsThePythonField`, `.RegshoIsThePythonField` (every payload byte, coverage bit for bit, NaN reasons, extras, stage files, source_checks block); pytest `test_holdings_engine_path.py` (flag-absent identity; real-exe identity through the Python wrapper) | @@PYT@@ |
| A1-SHIM | `1e4904b7` (R8): the four shims' `main` through the registry entry | `tools/test_field_registry.py` | @@PYT@@ |
| S-R2 | `1e4904b7` (R8): `same_panel` refuses None `PANEL_KEYS` statistics | `test_vendor_engine_path.py::test_panel_guard_fails_closed_on_missing_statistics` | @@PYT@@ |
| A3 review S-2 (reuse key / union) | `research_fields_cli.cpp` `build_entries(spec, role, to_build, planned)` (union of every planned field); reuse rule 6 `reuse.cpp` `history_differs` | `ResearchFieldsA4Reuse.RebuiltPriceFieldUsesTheWholeBuildsPanel`, `.PriceFieldReusedOnlyWithTheBuildsPanelHistory` | @@GT@@ |
| A3-3 (per-field re-hash under `--reuse`) | `reuse.cpp` `DigestCache` (one digest per distinct input per decision; `inputs_digested` = digests computed) | `ResearchFieldsA4Reuse.EachInputFileIsDigestedOncePerDecision` | @@GT@@ |
| S-6 boundary | `coskew_60m`, `xrd0_ttm`: registry rows `kind: python`, no C++ kind, not in `ENGINE_FIELDS` (verified) | registry pytests | @@PYT@@ |
| Teeth (TEETH-RESTORE) | `a4-evidence/teeth-a4.py`: five mutations (rule 6, union, digest-once, per-file late rule, ring eviction) in one build, bytes kept and restored by the tool, `git status --porcelain` asserted empty | `a4-evidence/teeth-@@TT@@.log` | @@TEETH@@ |

## Tests run (TEST-1: Debug only, named filters, own pytest files)

@@TESTS@@

Deferred to the gate (TEST-1): Release build and Release gtests; whole `atx-engine` test targets; whole
`scripts/tests` (seeds 0 / 1); `atx-engine/tools/test_stage_chain.py`.

## Builds
@@BUILDS@@

## Optional items
- Done: A3 re-review N-1 (R8, `1e4904b7`).
- Not done: OD-A3-SEAL pruning, A3 S-4 (`session_calendar_pin` per field), A3 concerns 3 / 4, ALSIG-3 (`ENGINE_FIELDS`
  does not route `gia_13f` / `russell_recon`), carried A1 / A2 minors, W1-MIN M-4.
- Scope kept Python (stated in the fixture pytest): `sv_offexchange_share126` (short_volume_ext) and `exch_up_365d`
  (listing switch), and the five 13F ownership fields (`inst_*`; AL-SIG's 13F reader exists, P18) - the fixture
  already holds their Python payloads for a later slice.

## New literals, keys, flags
- Spec keys of `atx.research-fields-spec/v1|v2`: `ftd`, `regsho_threshold`, `security_master` (`{dir,
  manifest_sha256}`), `shares_out` (`{path, sha256}`); each optional, absent = bytes unchanged.
- Reuse record key `inputs_digested` (only in a `reuse` block, i.e. only with `--reuse`).
- No new `atx.<name>/v<n>` schema literal.

## Cross-lane edits
- `atx-engine/CMakeLists.txt` tail (`target_sources(atx-engine-research-fields ...)`, six sources) and
  `atx-engine/tests/CMakeLists.txt` tail (three test sources): appends (R8, `5c53c2c9`).
- `sources/parquet_columns.{hpp,cpp}` (AL-SIG's file): two appended decoders.
- `tests/fixtures/research_fields/test_research_fields_engine_path.py`: the routes pin gains two names.

## For root: flag-absent identity and TRAIN identity
1. **Flag-absent identity.** The new spec keys and kinds are optional: a spec without `ftd` / `regsho_threshold` /
   `security_master` / `shares_out` parses and builds as before (the existing `ResearchFieldsCli.*`,
   `ResearchFieldsRegistry.*` gtests); the Python wrapper without `--engine-fields` is the plain builder
   (`test_holdings_engine_path.py::test_flag_absent_is_the_plain_builder`, the flag-absent tests of
   `test_research_fields_engine_path.py`). Rule 6 / the union (A3 S-2) only change REUSE decisions (a vendor price field
   from a panel of another history is recomputed, never carried); fresh-build bytes are unchanged. Expected
   byte-identical: every existing fields fixture payload and manifest (`ResearchFieldsFixture.*`, `test_vendor_*`).
2. **Gtests at the gate** (one target): `build-equity/bin/atx-engine-research-fields-tests.exe` whole, and the A4
   filter `ResearchFieldsA4*:ResearchFieldsHoldings*:ResearchFieldsRegistry*`. Pytests (own sessions):
   `atx-engine/tests/fixtures/research_fields_holdings/test_holdings_fixture.py`,
   `ATX_RESEARCH_FIELDS_EXE=<bin>/atx-research-fields.exe` + `test_holdings_engine_path.py`,
   `atx-engine/tests/fixtures/research_fields/test_vendor_engine_path.py` + `test_research_fields_engine_path.py`,
   `atx-engine/tools/test_field_registry.py`.
3. **TRAIN identity per moved field** (real data, root only; merge the pre-flip head first). For each of
   `ftd_shares_ratio21`, `regsho_threshold_days63` that the current TRAIN manifest lists: re-run the TRAIN recipe that
   wrote that manifest (same role, same field list so `shares_out` is this run's, same stage dirs; the stage pins are
   the manifest entry's `stage_manifest_sha256`) through the registry entry with the field routed to the engine:

   ```
   prepare_research_fields.py --registry atx-engine/tools/field_registry.json --fields <the TRAIN list>
     --role <TRAIN role> --role-sha256 <sha> --output <new dir> --engine-exe build-equity/bin/atx-research-fields.exe
     --ftd <dir> --ftd-sha256 <sha> --regsho-threshold <dir> --regsho-threshold-sha256 <sha>
     --security-master <dir> --security-master-sha256 <sha> <the TRAIN run's other argv>
   ```

   with the registry of the flip commit for that field (the entry routes only `kind: engine` rows; on the pre-flip head
   use `prepare_research_fields_engine.py --engine-fields <field> --engine-exe <exe> <the same argv>` with a registry
   copy whose row is flipped, as `test_holdings_engine_path.py` does). Pass: `<field>.f64` SHA-256 = the current
   manifest's `files` entry, the entry (minus `producer`) = the current entry, and the wrapper's guards (stage reads,
   stage checks, pins, sources) accept. Seal counts: `source_checks.holdings.ftd.rows_sealed` and the regsho lists /
   rows counts come from the engine receipt (`engine_fields/<field>.receipt.json`). Then merge the flip commits.
4. **Substitution list:** none (no path depth added; receipts under `engine_fields/` as for A3).

## Open questions
@@OPEN@@
