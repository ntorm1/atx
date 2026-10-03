# Lane A1 review

## Verdict
BLOCK

0 blocker, 1 major, 7 minor.

Spec compliance: ❌. One gap: as shipped, K-P9-1 refuses a `kind: engine` row that has no Python twin, and that row
is the DEC-5 path for every new field (M1). Everything else checks out (✅):
- the schema matches plan §2.3: 12 row keys, plus a top-level `dtype_rule`, which A2's reader tolerates;
- registration order equals manifest order, and v15's 84 names appear in v15 order;
- `dtype` is declared in the JSON and checked against the units rule, not the `grp_` prefix (`ea_time_of_day` is the
  one row where the two differ);
- one entry, `--registry`;
- holdings is a late `FIELD_MODULES` module;
- the freeze test exists;
- FD-2: runtime versions are recorded and enter the reuse key, and the IMPORTS-closure test exists;
- FD-5 / P13 (Python side): seal refusal in `load_prior`, and the key is renamed at publish.

The ruled deviations (A1-C conftest bind kept; P5 task 6 moved to A2) are honoured.

The producer identity claim reproduces independently, 41 of 41 SAME. The reasoning behind the v15 `--registry` claim is
sound: 82 fields reused; `nt_first_126` and `deal_pending` recomputed; 84 payloads equal. The predicted manifest diff
handed to root is incomplete (m4).

## Reviewed SHA
`88c9efa2156b7dee144d6c9f881bd46ddcd760ca`, base `d7c1c520`. Pool `C:/atx-wt/pool-12`, branch `feat/p9-a1-20261003`.
The tree was clean before and after (`git status --short --untracked-files=all` was empty). The only file written
outside scratch is this review.

## Evidence
Both commands ran in pool-12 with `PYTHONDONTWRITEBYTECODE=1` and `-p no:cacheprovider`. Each is one of the lane's
pytest commands.
```
$ cd C:/atx-wt/pool-12/atx-engine/tools && PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_field_registry.py test_no_new_python_builder.py test_field_module_imports.py test_prepare_research_fields_reuse_keys.py test_prepare_research_fields_seal.py
..............................                                           [100%]
30 passed in 13.74s
exit_code=0
$ cd C:/atx-wt/pool-12 && PYTHONDONTWRITEBYTECODE=1 "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tests/fixtures/research_fields
.......s..                                                               [100%]
9 passed, 1 skipped in 5.57s
exit_code=0
```

Spot checks. All were read-only; the scratch scripts live outside every tree.

- **Producer identity** (`fp_check.py`). For each item below, the base sources (`git show d7c1c520:atx-engine/tools/*.py`)
  were compared with the HEAD sources through HEAD's fingerprint functions:
  - the builder's `producer_fingerprints`;
  - `module_fingerprints` (module source plus builder host) for every bound module;
  - `imported_code` for every module that has it.

  Result: 41 lines, all SAME:
  - builder: finra, finra_sv, issuer, lake, role, th;
  - sec; holdings ×5; price ×5; v8 ×3;
  - v9 ×2, plus imported ×2;
  - xdata ×3, plus imported ×3;
  - gold; ohlc; ivshape;
  - mgr13f, divevent, deals and connected, each with its imported_code.

  The base builder blob is `e8b57af5`, as the report says.
- **Engine-only row probe**: append to the committed registry a row
  `{name: gia_13f, kind: engine, builder: gia_13f, ...}`.
  - `validate`: ok.
  - `check`: `RegistryError: ... rows the builder cannot produce: gia_13f`.
  - `generate(engine={...gia_13f})`: `RegistryError: engine rows gia_13f are not producible fields`.
- **Committed registry** (`git show 88c9efa2:atx-engine/tools/field_registry.json`): sha256 `6c56b739...3e51`, as
  reported. It has 92 rows, all `python`; 5 rows are `group`; `first_session` is null in every row. 17 rows embed
  `2024-01-01` in `spec_text`, exactly the 17 rows the report lists.

## Diagnostics
Running both suites in one process exits 1:
`pytest -q -p no:cacheprovider atx-engine/tools atx-engine/tests/fixtures/research_fields` gives
`1 failed, 379 passed, 1 skipped, 7 errors`.
- All 378 tools tests pass.
- The 7 errors are the window guard that already existed in `test_research_fields_engine_path.py:43` (present at the
  base).
- The 1 failure is A1's new assertion, `make_research_fields_fixture.py:167-169`: the builder was imported under the
  conftest window. See m5.

## Findings
path:line | severity | problem | required fix

- **M1** `atx-engine/tools/field_registry.py:283-299` (`check`), `:270-276` (`generate`),
  `atx-engine/tools/test_field_registry.py:86`, `:91-101` | **major** |
  - Problem: the tooling refuses a valid K-P9-1 `kind: engine` row whose field has no Python producer:
    - `check` requires `names(doc) == code_fields(ns)`;
    - `generate` only flips existing Python rows;
    - the committed-file test demands `generate(...) == file bytes` and pins `engine_flips ⊆
      prepare_research_fields_engine.ENGINE_FIELDS`, the three ported fields.

    DEC-5, and A1's own freeze docstring (`test_no_new_python_builder.py:3`), say a new field is a registry row
    naming a C++ builder kind. As shipped, that row breaks the one entry (`check` runs before any output) and turns
    the registry tests red. The wave-2 readers AL-SIG (`gia_13f`, `russell_recon`: plan §2.2 allows "append-only rows
    in the registries") and A3 (new price/ohlc kinds beyond the three in `ENGINE_FIELDS`) cannot use K-P9-1 without
    editing A1's loader and tests. The probe above reproduces the refusal. This is contract drift from K-P9-1.
  - Fix:
    - Compare only Python-producible rows with the code: their set and relative order, `builder`,
      `point_in_time`, `requires`, `dtype` and formula, as today.
    - Accept an engine row that is absent from the code when it validates; it needs no Python twin.
    - `entry` refuses such a row unless `--engine-exe` is given, since there is no Python fallback.
    - The committed-file test regenerates the Python rows and splices in the committed engine-only rows.
    - Drop or relax the `⊆ ENGINE_FIELDS` pin.
    - Add a test with a synthetic engine-only row: `check` accepts it, and `entry` refuses it without `--engine-exe`.
  - Alternative: a PM ruling that defers this to AL-SIG / A3 as named cross-lane edits of `field_registry.py` and
    `test_field_registry.py`. Cost: wave-2 lanes edit A1's contract code.
- **m1** `atx-engine/tools/field_registry.py:329-334` | minor |
  - Problem: `entry` passes every requested engine row to `engine.engine_path` without checking that the shim can
    route it (`parse_names` / `ENGINE_FIELDS`). With `--engine-exe`, an engine row the shim cannot route (for example
    a later A3 flip) is computed by Python silently: no stderr notice, and the user believes the exe produced it. The
    manifest's producer stays honest (Python).
  - Fix: when `--engine-exe` is given, refuse engine rows outside `prepare_research_fields_engine.ENGINE_FIELDS`, or at
    least print the existing "computed by the Python builder" notice for them.
- **m2** `atx-engine/tools/test_no_new_python_builder.py:22`, `:64-76` | minor |
  - Problem: the freeze is by filename only. Suppose a field module is named outside the pattern (for example
    `fields_sentiment.py`) and bound by a call-site one-liner:
    `import prepare_research_fields as b, fields_sentiment as s; b.FIELD_MODULES.append(s.bind(vars(b))); b.main(...)`.
    It adds Python fields without touching the registry. The plain builder (no `--registry`) accepts any bound module
    and never runs `check`. The registry-level guards only see modules the builder or the four shims bind.
  - Fix: add an AST scan. Any tracked `.py` outside `ALLOWLIST` that defines the field-module interface (module-level
    `PRODUCERS` and `bind`, cf. `MODULE_REUSE_INTERFACE`) fails the guard.
- **m3** `atx-engine/tools/prepare_research_fields.py:2983-2987` | minor |
  - Problem: `require_research_seal` treats a present but malformed `seal` (not a dict) as legacy-absent, so it is
    accepted with a log line. P13 accepts only an absent seal.
  - Fix: a manifest without a `seal` key, or with no `exclusive_end`, is legacy and logged. A `seal` that is not an
    object, or an `exclusive_end` that is not a string, raises `SealError`. Add one case to
    `test_prepare_research_fields_seal.py`.
- **m4** `.superpowers/sdd/platform-p9-20261003/task-A1-report.md:206-212` | minor |
  - Problem: the expected diff of the root identity run is incomplete.
    - v9 is a partially reused module group: `earn_season_rank` is reused and `nt_first_126` is recomputed.
      `merge_module_reuse` (`prepare_research_fields.py:2924-2930`) therefore moves v15's whole `v9` check into
      `reuse.prior_source_checks_of_partial_groups.v9`. `research_fields_v9.py:558` writes `source_checks.v9` with
      `nt_first_126` only, so `source_checks.v9.earn_season_rank` disappears; it is not renamed in place.
    - `deals` changes from carried (`source_checks_from_prior`) to computed.

    A re-pin ruling written from the listed paths would mis-describe the manifest.
  - Fix: amend "How root verifies": `source_checks.v9` shrinks to `nt_first_126`; v9's prior check moves under
    `reuse.prior_source_checks_of_partial_groups` (renamed key); `source_checks_from_prior` loses any recomputed group.
- **m5** `atx-engine/tests/fixtures/research_fields/make_research_fields_fixture.py:167-169` | minor |
  - Problem: the new window assertion makes `pytest atx-engine/tools atx-engine/tests/fixtures/research_fields` in one
    process fail (see Diagnostics), next to the engine-path guard that already existed. The lane's evidence runs the
    suites separately, but nothing tells root or T1 (CTest / pytest registration) to do the same.
  - Fix: state in the report's "How root verifies" that the two suites run in separate processes, or make the fixture
    test refuse with the same explicit `pytest.fail` message as the engine-path guard.
- **m6** `atx-engine/tools/prepare_research_fields_draft.py:41-45` (and the ohlc / xdata / ydata shims) | minor |
  - Problem: the shims' `main` is still `register(vars(builder)); builder.main(argv)`, not a wrapper over the
    `--registry` entry, which brief item (2) asks for. The lane gives a reason: the window-dependent formulas for the
    SEC clocks refuse under the conftest bind. The deviation is flag-absent safe, but it is not ruled.
  - Fix: a PM ruling accepting it for wave 1, with re-routing (or deletion) owned by T2 / E2 once the bind is gone.
- **m7** `atx-engine/tools/field_registry.py:101-103` (`validate`) | minor |
  - Problem: `validate` accepts any string for `first_session`, while A2's C++ reader
    (`field_registry.cpp:89-94` at `2b6f8e6f`) requires a strict `YYYY-MM-DD`. A registry that the Python loader
    accepts can be refused by the exe, so the two K-P9-1 readers disagree.
  - Fix: validate `first_session` as null or a strict `YYYY-MM-DD` date in Python too, and add one refusal case to
    `test_malformed_documents_are_refused`.

Assessed and not findings:
- **IMPORTS completion.** It moves only the `names` list inside `imported_code` for v9_nt and y_deal; the closure
  SHA-256 values are unchanged (fp_check). The affected fields are `nt_first_126` and `deal_pending`; `earn_season_rank`
  is unaffected (its IMPORTS did not change). No row requires either field, so nothing cascades. The added names are
  read by the closures.
- **Recomputed payloads.** They are expected to be byte-equal: the code is fingerprint-identical and the inputs are the
  same, and both fields are indicators with no float reduction.
- **Rename at publish.** No code reads the legacy key from a prior manifest; prior checks are only carried. Legacy
  manifests therefore still load, and the C++ CLI key (A2) passes through `published_checks`.
- **Holdings LATE refactor.** It is equivalent to the old publish hook: same order, the same reuse merge with the
  `_source_checks` pop scoped to holdings, and `check` runs before output. The existing holdings tests pass unchanged.
- **`.gitattributes`.** It affects only `atx-engine/tools/field_registry.json`. `-text` is EOL-only and leaves diff
  and merge as they were.
- **Window dependence of the 17 SEC-clock formula hashes.** It existed before this lane (the clock text embeds the
  seal). The registry captures it, and `check` refuses a registry generated under another window before any output.
  The committed file stays unusable inside the conftest-bound tools harness until T2.
- **`runtime_versions` and reuse.** The key is added unconditionally, and the reuse block gains `runtime_rule` and
  `prior_runtime_versions`. Plan §1.2 and A1's root check anticipate both ("moves manifest bytes", one re-pin).
  Reuse is all-or-nothing on exact versions, by FD-2 design.

## Checked
- [x] `.agents/cpp/agent.md` §10: not applicable. The diff contains no C++; the C++ fixture key consumer
  (`research_fields_fixture_test.cpp:170,197`) is A2's under P5, and the A1-before-A2 merge order was declared.
- [x] The diff stays inside the brief's files-in-scope, P5 and A1-C, with two notes:
  - `test_research_fields_fixture.py` changed (a necessary consequence of the P5 rename);
  - `atx-engine/tools/.gitattributes` is new (harmless).

  Nothing touches `scripts/**`, `atx-db/`, C++ or builder arithmetic.
- [x] The evidence in the report matches its claims:
  - the 30-test command was re-run (30 passed);
  - the fixtures suite was re-run (9 passed, 1 skipped);
  - the tools tests pass, 378, inside the combined run;
  - 41 of 41 producer fingerprints are SAME (reproduced);
  - the registry sha256 and its 17 window-dependent rows match.
- [x] Seal per P13: a present and different seal is refused (fresh-interpreter test; refused before any payload
  copy); an absent seal is logged and accepted; `load_prior` is wired for both the builder and the modules' reuse.
  The blindness rules held: only synthetic fixtures and the committed registry were opened.
