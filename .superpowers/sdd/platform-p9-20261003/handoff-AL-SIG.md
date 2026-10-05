# Handoff: lane AL-SIG (owner stop)

- Pool: `C:/atx-wt/pool-13` (reused, no lease call). Branch: `feat/p9-al-sig-20261003` (the dispatch contract's
  `feat/p9-<lane-lowercase>-20261003`; the brief's `feat/p9-alsig-20261003` was not used). Base: `1239a5ff`.
- Head: the WIP commit `2d7aba4e` ("wip(al-sig): ..."), then the commit that adds this file.
- Nothing has been compiled (lanes never build C++). The pytest suites touched pass (below). No process left running
  by this lane (the python.exe processes visible at the stop were not started by it).

## Brief / carry items

| item | state |
|---|---|
| Carry P18: 13F and Russell-index source readers, K-P9-2 style, under `research/fields/sources/` (ruling 18) | done (uncompiled) |
| `gia_13f` C++ builder kind + engine-only registry row (DEC-5) | done (uncompiled) |
| `russell_recon` C++ builder kind + engine-only registry row (DEC-5) | done (uncompiled) |
| planted-leak probe with teeth per builder | done: `ResearchFieldsAlSigGia.PlantedLeakProbeHasTeeth`, `ResearchFieldsAlSigRussell.PlantedLeakProbeHasTeeth`, plus the sealed-part probe `ResearchFieldsAlSigThirteenf.SealedPartIsNeverOpenedAndTheProbeHasTeeth` |
| <= 10 frozen DSL strings with theme, tier, sign in string, horizon class + half-life (PM8-5), citation, K-P9-11 keys | done: 2 queue candidates `scripts/specs/p9/candidates/{gia_13f,russell_recon}.json` |
| `lazy_prices`, `hf_crowd` as `blocked_on_data` registrations | done: `scripts/specs/p9/blocked/{lazy_prices,hf_crowd}.json` |
| ALSIG-FLIP: no existing field flipped to `kind: engine` | kept (only two new engine-only rows appended) |
| Lane report `task-AL-SIG-report.md` | **not started** |
| Final self-review pass of the C++ for `/W4 /WX` traps | **partial** (see "Where I stopped") |

## Where I stopped and the next steps

1. I had just re-wrapped two over-long lines in `atx-engine/tests/fixtures/research_fields_al_sig/make_al_sig_fixture.py`
   (the module docstring, and `oracle_basis`: the generator expression split into `both = [...]` then
   `ratios = sorted(...)`). The generator was **not re-run** after that edit. Next: run it into a scratch dir
   (`python make_al_sig_fixture.py --out <scratch>`) and check that every output file is byte-identical to the
   committed fixture (it should be: the edit does not change behaviour). If a byte differs, the committed fixture
   wins: revert the edit, never the expected files. Optional: an IDE mypy note at line 250 (a `None` security_id
   in a list typed by inference) is cosmetic; annotate `out: list[tuple]` if you touch it.
2. Every C++ line is now <= 100 columns (scanned). Files re-read in full for compile traps: `stage.cpp`,
   `thirteenf_filings.cpp`, `thirteenf_holdings.cpp`, `russell_proxy.cpp`, `russell_recon_field.cpp`,
   `gia_13f_field.cpp`, `registry.cpp`, all five test files. Not re-read since they were written before the context
   reset: `gia_13f_skill.cpp`, `parquet_columns.cpp`, `gia_13f_field.hpp`, `stage.hpp`. Next: one read of those four
   for unused variables, sign conversions, shadowing, narrowing and header/signature agreement.
3. Write `.superpowers/sdd/platform-p9-20261003/task-AL-SIG-report.md` (TEMPLATES.md "Lane report" shape; the
   contents of this file are most of it) and commit with `git add -f`. Then the short final message to the PM.

## Files

Owned (new): `atx-engine/include/atx/engine/research/fields/sources/{stage,parquet_columns,thirteenf,russell_proxy}.hpp`,
`atx-engine/src/research/fields/sources/{stage,parquet_columns,thirteenf_filings,thirteenf_holdings,russell_proxy}.cpp`,
`atx-engine/include/atx/engine/research/fields/{gia_13f_field,russell_recon_field}.hpp`,
`atx-engine/src/research/fields/{gia_13f_field,gia_13f_skill,russell_recon_field}.cpp`,
`atx-engine/tests/research/research_fields_al_sig_{support.hpp,thirteenf_test.cpp,gia_test.cpp,russell_test.cpp,fixture_test.cpp}`,
`atx-engine/tests/fixtures/research_fields_al_sig/` (generator `make_al_sig_fixture.py`, `.gitattributes` `* -text`,
role/, thirteenf/, indexes/, expected/), `scripts/specs/p9/candidates/{gia_13f,russell_recon}.json`,
`scripts/specs/p9/blocked/{lazy_prices,hf_crowd}.json`, `scripts/tests/test_p9_al_sig_registrations.py`.

Cross-lane (minimal; list them in the report):
- `atx-engine/include/atx/engine/research/fields/build_spec.hpp` (A2): `struct StageRequest {dir, manifest_sha256}` and
  two tail members `std::optional<StageRequest> thirteenf, indexes`.
- `atx-engine/include/atx/engine/research/fields/registry.hpp` (A2): includes `sources/stage.hpp`; `SharedSources`
  gains `std::optional<PinnedStage> thirteenf, indexes`; declares `check_kind_request` (forwarder of the ported kinds'
  request check); doc comment lists the two kinds.
- `atx-engine/src/research/fields/registry.cpp` (A2): kind table 3 -> 5 (`gia_13f`, `russell_recon` appended), the
  `check_kind_request` forwarder at the end.
- `atx-engine/src/research/fields/research_fields_cli.cpp` (A2): spec keys `thirteenf` / `indexes`
  (`{dir, manifest_sha256}`, pin required, compared lower-case); `parse_reuse` re-expressed on two shared helpers
  (`sha256_pin`, `pinned_dir`) with the same messages and values.
- `atx-engine/tests/research/research_fields_registry_test.cpp` (A2): `KindLookup` now expects 5 kinds and gives the
  parse spec dummy stage requests.
- `atx-engine/tools/field_registry.json` (A1/A2): two engine-only rows appended in `field_registry.dump` form.
- `atx-engine/CMakeLists.txt`, `atx-engine/tests/CMakeLists.txt`: one `target_sources` block appended at each tail.
- A3's files (the field writer) were not touched.

## Tests written and last result

- gtests (written, never compiled or run): `ResearchFieldsAlSigThirteenf.{QuarterCalendar, EffectiveFilingsRule,
  IndexVisibilityDeadlineAndSeal, RowScreen, QuarterPositionsClosedForm, BasisFactorsDetectASplit,
  StagePinAndContract, SealedPartIsNeverOpenedAndTheProbeHasTeeth}`, `ResearchFieldsAlSigGia.{CenteredRanksClosedForm,
  HoldingsReturnsClosedForm, HolderScoresClosedForm, RowClockAnchorStalenessNoSkipBack,
  AccumulatorVisibilityAndManagerFloor, PlantedLeakProbeHasTeeth}`, `ResearchFieldsAlSigRussell.{SnapshotsFromRows,
  UndeclaredIndexIdsAreRefused, MovesClosedForm, EventWindowClock, PlantedLeakProbeHasTeeth}`,
  `ResearchFieldsAlSigFixture.{CommittedRowsPinTheSpecs, BuildMatchesTheOracle, ReuseFollowsTheStagePin,
  KindsRefuseWithoutTheirStage, SpecKeysParseAndRefuseBadPins}`; edited `ResearchFieldsRegistry.KindLookup`.
- pytest (all exit 0): `atx-engine/tools/test_field_registry.py` + `atx-engine/tools/test_no_new_python_builder.py`
  39 passed; `scripts/tests/test_p9_al_sig_registrations.py` + `scripts/tests/test_wave_queue.py` 13 passed;
  `research_cycle.py candidates validate --dir scripts/specs/p9/candidates` "2 valid".
- Root verifies: build `atx-engine-research-fields-tests` and `atx-research-fields` (wrapper, target-scoped); run
  gtest filters `ResearchFieldsAlSig*`, `ResearchFieldsRegistry*`, and `ResearchFields*` for the existing fixtures'
  byte identity (flag-absent: a v1 / v2 spec without `thirteenf` / `indexes` builds exactly as before; the three
  ported kinds are untouched); K1 `--plan-only` of `rank(gia_13f)` and `rank(russell_recon)` on the fields build that
  will carry P9-S.

## Decisions (and why)

- Readers split into a generic `PinnedStage` (manifest pin, schema / status / rule keys, per-file bytes + SHA-256
  before and after the read, file stamps) and typed pure decoders + pure logic, so the logic is gtested without a
  parquet writer (atx-core cannot write DATE32; the fixture parquet is written by pyarrow with an independent numpy
  oracle that reuses the house Python 13F helpers).
- Each kind's entry `sources` is the stage manifest only (it pins every stage file); reuse rule 5 re-hashes exactly
  that, so reuse follows the stage pin (`ReuseFollowsTheStagePin`).
- Seal: sealed holdings parts are never opened (counted), a quarter whose V(P) is at/after the seal is never used,
  Russell rows available at/after the seal are dropped (counted).
- Russell index ids `r1000` / `r2000` are declared, not discovered; any other spelling is refused listing the ids
  seen. The indexes manifest is assumed to carry an alpha-panel `files` block (both from the house docs, not data).
- gia constants fixed blind in `GiaParams` (8 / 6 quarters, 5 holders, 3 managers, half coverage, ratio screen 10,
  the stage's 150-day staleness). Clock: V*(P) = running max visibility; read when V*(P) < previous session 22:00 UTC.
- Strings: `rank(gia_13f)` (ownership_flow, C+, slow, half-life 63, WYZ 2012, sample end 2006, H) and
  `rank(russell_recon)` (ownership_flow, C+, medium, half-life 21, CHL 2015, sample end 2012, H); plain `rank` as the
  closest existing 13F member `stio_trade`. Horizon class / half-life live in `notes` (no structured key exists).
- `blocked_on_data` registrations are kept out of the candidate queue (a queue status cannot be `blocked_on_data`),
  in `scripts/specs/p9/blocked/`, validated by `wave_manifest.candidate_problems` in the lane pytest.
- No cell template under `scripts/specs/p9/templates/`: the brief does not list one and screening is root's P9-S.
- Registry-less (v1) specs now also accept the two new kind names (the kind table is the v1 name list); existing v1
  builds are unchanged.

## Open questions for the PM

- WYZ 2012 `source_sample_end` = 2006 is from memory (the literature review marks the sample "pre-2010 [verify]").
- gia_13f's literature rho estimate (.15 to the themes) is above the lit section 3.0 bar of .1; it is registered on
  the mechanism argument, stated as such in its notes.
- The Python engine shim (`prepare_research_fields_engine.ENGINE_FIELDS`) does not route engine-only rows, so the
  two fields are buildable only by running `atx-research-fields build --registry` directly with a v2 spec carrying
  `thirteenf` / `indexes`; routing them through `prepare_research_fields.py --registry --engine-exe` is a shim change
  outside this lane.

## Traps for the next agent

- Never write files through shell heredocs (the hook breaks them); use Write / Edit.
- `.mypy_cache/` and `__pycache__/` appear under the fixture dir (an IDE runs mypy); both are gitignored, never add
  them. Stage files by explicit path, not `git add -A`.
- The fixture's `parts/source=2023q4` and `source=2024q1` holdings files are deliberately mis-pinned poison (listed
  with an all-zero SHA-256); the sealed-part probe depends on them. Do not "fix" their manifest entries.
- Never edit `expected/` files or `counts.json` to make a test pass; regenerate only by running the generator, and
  then re-check every pin.
- `field_registry.json` is pinned byte for byte (`fr.dump` form, LF, `-text`); append rows only through
  `field_registry.dump`, and `formula_sha256` must equal the C++ `formula_sha256(spec)` of the kind's spec text
  (`ResearchFieldsAlSigFixture.CommittedRowsPinTheSpecs` checks both).
- Test Python file names must not match `(prepare_)?research_fields_[^/]*\.py` (the builder freeze test).
- Inside a lambda passed to `ATX_TRY_VOID`, use `ATX_TRY` only (ATX_TRY_VOID uses a fixed temp name).
- gtest: compare `std::filesystem::path` values as `.string()`.
