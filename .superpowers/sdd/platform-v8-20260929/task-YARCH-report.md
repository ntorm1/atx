# Task YARCH report: platform core audit, migration plan, slices 1 and 2 (Ruling PM8-12)

Lane YARCH, worktree `C:/atx-wt/pool-16`, branch `feat/platform-v8-yarch-20261002`, base `798d3b23`.
Status: all five items done. 4 code/doc commits plus this report; 53 files, +4,863 lines, nothing existing
modified except two additive CMake blocks. No C++ was compiled by the lane (lane rule 2); root's build is the
first compile.

| Item | Commit | What |
|---|---|---|
| 1 audit | `e6f6e72d` | `docs/plans/2026-10-02-platform-core-audit.md` |
| 2 migration plan | `2288dbc1` | `docs/plans/2026-10-02-platform-core-migration.md` |
| 3 slice 1 (C++) | `eccf6338` | library `atx-engine-research-fields` + test target `atx-engine-research-fields-tests` |
| 4 slice 2 (spec generator) | `18e71313` | `atx-impl/strategies/generate_from_spec.py`, `specs/library-v71.json`, tests |
| 5 report | this commit | this file |

## 1. Audit (findings)

Python research surface outside tests is 37,997 lines in 66 modules (tests: 68 files, 28,989 lines):

* class B (logic the engine should own; Python duplicates or replaces C++): 27 modules, 22,352 lines (58.8%)
* class A (thin wrappers / orchestration that should stay Python): 28 modules, 10,576 lines (27.8%)
* class C (frozen versioned copies): 11 modules, 5,069 lines (13.3%)

Duplicated implementations: 3,378 lines. The rules written more than once include the as-of clock rule (3x),
the numpy coverage statistics, composition rules mirrored between Python and C++, trial counting and integrity
statistics, and the admission screens. The `generate_fund_ic_v*` lineage is
`v4 -> v5 -> v6 -> v61 -> v70 -> v71` (v42 a dead branch); every step copies loader/parser/validator/recipe code
and appends a wave of data; `generate_fund_ic_v71.py --check` executes 3,726 lines to verify two files.

Top 5 migrations by (lines retired x risk removed) / effort:

| Rank | Migration | Lines retired | Score |
|---:|---|---:|---:|
| 1 | legacy library generators -> one spec-driven generator (slice 2, done) | 5,069 | 10.1 |
| 2 | research field builders -> engine field library (slice 1 started) | 8,891 | 8.9 |
| 3 | composition-rule Python mirrors -> engine composition | 1,756 | 5.3 |
| 4 | integrity statistics + trial counting -> engine ledger | 1,953 | 2.9 |
| 5 | admission screens -> engine admission | 1,520 | 2.3 |

## 2. Migration plan (summary)

Target layout `atx-engine/{include,src}/atx/engine/research/{fields,composition,admission,ledger}`; Python keeps
roles A (CLI, orchestration, reports). Wrapper contract: executables + JSON spec in / JSON receipt out (no new
pybind11 surface: `atxpy` builds via scikit-build + pip pybind11, outside vcpkg, and a binding per builder costs a
second ABI to keep equal). Identity discipline: every port lands beside the Python, behind an explicit flag
(slice 3: `prepare_research_fields.py --engine-fields NAME --engine-exe PATH`), with payload-byte identity on a
committed fixture produced by the existing Python. Slices 1-10 each name their build targets.

## 3. Slice 1: engine field-builder library (built by root, not by the lane)

`atx-engine/include/atx/engine/research/fields/` + `atx-engine/src/research/fields/`, namespace
`atx::engine::research::fields`, 11 headers / 12 sources, links `atx::core` (PUBLIC) and `nlohmann_json` (PRIVATE)
only; depends on header-only `research_window.hpp` for the seal (no hard-coded seal date or TRAIN end).

* `clock` civil days, strict ISO dates, reader-side seal; `role_axes` / `role_rows` the pinned research role and
  hashed, session-ordered row streams; `field_writer` / `field_stats` exclusive-create payload, canonical NaN and
  the manifest coverage block with numpy-exact pairwise sum, linear quantile and `round(x, 6)`; `field_spec`
  canonical formula document + `formula_sha256` equal to the Python's.
* `asof_series`: the as-of clock rule (latest observation with available_day < session day, 45-day staleness,
  visible NaN stays NaN) that is written three times today.
* Builders ported: `vol_126` (price/volume, `trailing_mean` + `volume_mean_field`) and `si_shares` / `si_dtc`
  (publication-lagged FINRA as-of, `finra_inputs` + `finra_asof_field`: producer receipt, strict CSV contract,
  dissemination-date check, seal drop, unknown-id count, vintage-risk block).

Tests `atx-engine/tests/research/` (26 gtests, 5 suites): `ResearchFieldsClock` (7), `ResearchFieldsWriter` (6),
`ResearchFieldsVolumeMean` (5), `ResearchFieldsFinraAsof` (5), `ResearchFieldsFixture` (3). Closed-form fixtures;
a look-ahead probe per builder that holds on the real rule and flags a planted leaky variant (`vol_126`: push
before read; FINRA: `<=` instead of `<`); formula fingerprints pinned to the Python values (vol_126 `cb44b19e...`,
si_shares `0d8dedae...`, si_dtc `051996be...`); byte identity on the committed fixture
`atx-engine/tests/fixtures/research_fields/` (300 synthetic sessions, 8 ids, a sealed probe row, an unknown id,
a CRLF CSV) whose expected outputs were written by running the EXISTING `prepare_research_fields.py`
(`make_research_fields_fixture.py`; its pytest re-runs the Python and compares every byte). Expected payload
sha256: vol_126 `ef4a61eb...`, si_shares `51978fbb...`, si_dtc `edd6256a...`.

## 4. Slice 2: one spec-driven generator

`atx-impl/strategies/generate_from_spec.py` (185 lines) reads an `atx.library-spec/v1` spec and drives
`generate_library.py`'s builders (imported, not copied). `specs/library-v71.json`:

* generates `fund_industry_ic_v71.json` (`787c802e...`, 39,317 bytes: the bytes `generate_fund_ic_v71.py` wrote)
  and `fund_industry_ic_v71.recipe.v2.json` (`69e95298...`, 25,389 bytes) from `alphas/registry.json` +
  `libraries/v71.json`;
* pins the registry's house budget as v7.1 recorded it (`max_roster` 56; the registry is at 80), which repairs the
  drift that makes `generate_library.py --library v71 --check` fail at the base, without editing either file;
* freezes by sha256 all 19 artefacts the eleven class-C files wrote (v4..v71 libraries and legacy recipes,
  price_volume_ic96_v2, pv_fields_ic121_v3, slow_price_volume_ic48_v1). The legacy recipes embed their own
  generator's source hash, so they are provenance documents, not regenerable outputs;
* takes the K1 static validation from the IC exe (`plan.exe`) or a saved `--plan-only` output (`plan.json`; the
  committed spec uses the hand-written `alphas/fixtures/v71_plan_k1.json`).

`test_generate_from_spec.py` (5 tests): the v71 spec `--check` and `--out` (byte-identical to the committed files);
the whole lineage is frozen or generated; on a synthetic tree the spec's artefacts equal byte for byte what
`generate_library.py` writes (library `d7252f3d...`, recipe `818269808...`); the plan from a stand-in IC exe (argv
and library sha256 checked); refusals (pin, frozen, unpinned under `--check`, plan row, roster cap, spec shape).

The eleven class-C files and their tests are DEPRECATED (audit section 4) and left in place. They are deleted in one
commit only after root's identity run below exits 0.

## 5. How root verifies

C++ (root's build lane; the lane never compiled):

```
powershell scripts\atx-build.ps1 build atx-engine-research-fields-tests
<build dir>\bin\atx-engine-research-fields-tests.exe --gtest_filter=ResearchFields*
```

Expected: 26 tests from 5 test suites, all passed. Both targets are `EXCLUDE_FROM_ALL`; no existing target links
or compiles the new code, so every existing build and test is unchanged. Single-TU shaping, if the first build
fails: `powershell scripts\atx-build.ps1 check atx-engine\src\research\fields\<file>.cpp`.

Python (synthetic data only; all run by the lane, all passing):

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tests/fixtures/research_fields/test_research_fields_fixture.py
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/strategies/test_generate_from_spec.py atx-impl/strategies/test_generate_library.py
```

Lane results: fixture 2 passed; generator 5 passed + generate_library 9 passed (14 passed).

Slice 2 identity run (gate for deleting the class-C files), metadata only, no payload read:

```
atx-equity-strategy-ic --plan-only --library atx-impl/strategies/fund_industry_ic_v71.json --library-sha256 787c802ed1cdf7cfc507500b014525c3d4dff148f44e77ebd8368c0a686c2259 --train <role manifest> --train-sha256 <pin> --train-fields <fields-v9 dir> --train-fields-sha256 <pin> > v71_plan_live.json
"C:/Program Files/Python312/python.exe" atx-impl/strategies/generate_from_spec.py --spec specs/library-v71.json --check --plan-json v71_plan_live.json
```

Expected exit 0 and `frozen: 19 artefacts verified; plan (v71_plan_live.json): 48 K1 rows within budget`.
Without `--plan-json` the committed fixture plan is used (the lane's run: exit 0).

## 6. Deviations

* No C++ compiled (lane rule 2). Files were formatted with clang-format 18.1.8 and the repo `.clang-format`
  (ColumnLimit 100); the code token stream before and after formatting was compared and is identical.
* The filing-lagged builder is the FINRA as-of pair (si_shares, si_dtc), not an SEC filing field: it is the
  shortest publication-lagged builder and the as-of rule it carries is the one written three times.
* Identity claim of slice 1 is payload bytes + every coverage number (bit for bit) + source checks + vintage block +
  formula fingerprint. The Python manifest's bytes are not claimed (they carry the Python code identity and absolute
  paths); the manifest writer is slice 3.
* Slice 2 regenerates the v7.1 library and slim recipe; the legacy recipes are frozen, not regenerated (they hash
  their own generator). The committed spec's plan is the hand-written K1 fixture until root supplies the live one.
* New directory `atx-impl/strategies/specs/` (library specs) is distinct from `scripts/specs/` (run specs).

## 7. Cross-lane edits

* `atx-engine/CMakeLists.txt`: one additive block (new library) after `target_link_libraries(atx-engine ...)`,
  before `add_subdirectory(tests)`. YOPS also edits this file (source list of `atx-engine`); different region, no
  textual overlap expected.
* `atx-engine/tests/CMakeLists.txt`: one additive block (new test executable) before
  `atx-engine-w1-data-tests`.
* `atx-engine/tests/fixtures/research_fields/*.csv` force-added past the repo-wide `*.csv` ignore; fixture
  `.gitattributes` `* -text` keeps the pinned bytes (one CSV is CRLF on purpose).
* No file owned by YSIG, YDATA, YCOMB, YINFRA or YOPS was touched; nothing under `atx-db/`.

## 8. Open risks

* First compile is root's: the lane wrote for clang-cl /W4 /permissive- /WX by reading the owning idiom; a
  warning-as-error on first build is possible (fix with `check <file>`).
* numpy coupling: the coverage statistics reproduce numpy 1.26.4 (pairwise sum, linear quantile `_lerp`); a numpy
  upgrade on the Python side could move the Python's numbers. The fixture pytest catches it.
* Signed-zero ties: where numpy's SIMD min / max / partition picks between -0.0 and +0.0 the engine may pick the
  other; payload bytes are unaffected, a coverage min/max/quantile of exactly zero could differ in sign. Not
  exercised by the fixture.
* The exclusive-create writer uses `_wfopen_s(..., L"wbx")` on Windows and `fopen(..., "wbx")` elsewhere.
* Deleting the class-C files before root's identity run would remove the only program that re-derives the legacy
  recipes' content; after the run the frozen sha pins are the record.
* Slice 3 (engine exe `atx-research-fields` + the `--engine-fields` flag in `prepare_research_fields.py`) is not
  started; until then nothing in production calls the new library.
