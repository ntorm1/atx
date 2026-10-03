# Lane AL-SIG report

## Outcome

DONE_WITH_CONCERNS. The 13F and Russell-index stage readers (carry P18) are in, and so are the engine-only builder kinds
`gia_13f` and `russell_recon` with their registry rows and planted-leak probes. One queue string, `rank(russell_recon)`,
is registered, plus two `blocked_on_data` registrations. `rank(gia_13f)` is excluded by the bar (ruling ALSIG-2). The
C++ was built by the lane (W2-BUILD). All 24 new gtests pass. The only reds in the target are the two pre-existing
M1a-RED gtests that A3 owns. The concerns are listed at the end.

## Branch / SHA

- Pool `C:/atx-wt/pool-13` (reused, no lease call). Branch `feat/p9-al-sig-20261003`. Base `1239a5ff`.
- Commits: `2d7aba4e` (readers, kinds, rows, gtests, registrations), `80074679` (handoff at owner stop), `f1b3c792`
  (compile fix: `russell_recon_field.cpp` includes `clock.hpp` for `iso_date`), `34e12223` (ALSIG-2: `rank(gia_13f)`
  leaves the queue). The commit that adds this report comes after them. Everything is committed and the tree is clean.

## Files changed

Owned (new):
- `atx-engine/include/atx/engine/research/fields/sources/{stage,parquet_columns,thirteenf,russell_proxy}.hpp`
- `atx-engine/src/research/fields/sources/{stage,parquet_columns,thirteenf_filings,thirteenf_holdings,russell_proxy}.cpp`
- `atx-engine/include/atx/engine/research/fields/{gia_13f_field,russell_recon_field}.hpp`
- `atx-engine/src/research/fields/{gia_13f_field,gia_13f_skill,russell_recon_field}.cpp`
- `atx-engine/tests/research/research_fields_al_sig_{support.hpp,thirteenf_test.cpp,gia_test.cpp,russell_test.cpp,fixture_test.cpp}`
- `atx-engine/tests/fixtures/research_fields_al_sig/`: generator `make_al_sig_fixture.py`, `.gitattributes`
  (`* -text`), and the `role/`, `thirteenf/`, `indexes/` and `expected/` directories. All of it is synthetic and
  written by pyarrow, with an independent numpy oracle.
- `scripts/specs/p9/candidates/russell_recon.json`, `scripts/specs/p9/blocked/{lazy_prices,hf_crowd}.json`,
  `scripts/tests/test_p9_al_sig_registrations.py`.
- `scripts/specs/p9/candidates/gia_13f.json` was added in `2d7aba4e` and removed in `34e12223` (ALSIG-2), so the net
  diff from the base does not contain it.

Cross-lane edits (each is minimal; the list is for root at merge):
- `atx-engine/include/atx/engine/research/fields/build_spec.hpp` (A2): adds `struct StageRequest {dir,
  manifest_sha256}` and two tail members `std::optional<StageRequest> thirteenf, indexes`.
- `atx-engine/include/atx/engine/research/fields/registry.hpp` (A2):
  - includes `sources/stage.hpp`;
  - `SharedSources` gains `std::optional<PinnedStage> thirteenf, indexes`;
  - declares `check_kind_request`, a forwarder to the ported kinds' request check;
  - the doc comment lists the two new kinds.
- `atx-engine/src/research/fields/registry.cpp` (A2): the kind table goes from 3 to 5 entries (`gia_13f` and
  `russell_recon` appended). The `check_kind_request` forwarder is added at the end of the file.
- `atx-engine/src/research/fields/research_fields_cli.cpp` (A2):
  - new spec keys `thirteenf` and `indexes`, each `{dir, manifest_sha256}`; the pin is required and compared
    lower-case;
  - `parse_reuse` is re-expressed on two shared helpers (`sha256_pin`, `pinned_dir`), with the same messages and
    values as before.
- `atx-engine/tests/research/research_fields_registry_test.cpp` (A2): `KindLookup` now expects 5 kinds and gives the
  parse spec dummy stage requests.
- `atx-engine/tools/field_registry.json` (A1/A2): two engine-only rows appended in `field_registry.dump` form.
- `atx-engine/CMakeLists.txt` and `atx-engine/tests/CMakeLists.txt`: one `target_sources` block appended at the tail of
  each.
- A3's files (the field writer) were not touched.

## Required items (brief + carry)

| item | result |
|---|---|
| Carry P18: 13F and Russell-index source readers, K-P9-2 style, under `research/fields/sources/` (A4 reuses the 13F reader) | Done. `PinnedStage` pins the manifest (SHA-256, schema, rule keys) and checks every file's bytes and SHA-256 before and after it is read. Typed pure decoders and pure logic sit on top of it. |
| `gia_13f` C++ builder kind + engine-only registry row (DEC-5) | Done. The field and its row stay. The DSL string is not registered (ALSIG-2, below). |
| `russell_recon` C++ builder kind + engine-only registry row (DEC-5) | Done. |
| Planted-leak probe with teeth for every builder | `ResearchFieldsAlSigGia.PlantedLeakProbeHasTeeth`, `ResearchFieldsAlSigRussell.PlantedLeakProbeHasTeeth`, and the sealed-part probe `ResearchFieldsAlSigThirteenf.SealedPartIsNeverOpenedAndTheProbeHasTeeth`. All pass. |
| At most 10 frozen DSL strings, each with theme, tier, sign in the string, horizon class + half-life (PM8-5), citation and the K-P9-11 keys | 1 queue string, `rank(russell_recon)`, plus 2 blocked registrations. The 10-string limit is pinned by the test. |
| `lazy_prices`, `hf_crowd` registered only as `blocked_on_data` | Done, in `scripts/specs/p9/blocked/`. They are not in the queue: a queue status cannot be `blocked_on_data`. |
| ALSIG-FLIP: no existing field flipped to `kind: engine` | Held. Only the two new engine-only rows were appended. |
| Ruling ALSIG-1: verify the WYZ `source_sample_end` | **Verified: 2006** (evidence below). Because of ALSIG-2 the value is no longer in any registration. CHL's 2012 (`russell_recon`) was checked the same way and is also verified. |
| Ruling ALSIG-2: `gia_13f` not registered; record it as excluded by the bar | Done in `34e12223`. `scripts/specs/p9/candidates/gia_13f.json` is removed. `test_string_excluded_by_the_bar_is_not_registered[gia_13f]` pins that the id and `rank(gia_13f)` are in neither the queue nor the blocked set, and that the engine-only field row still exists. |
| Ruling ALSIG-3: `ENGINE_FIELDS` routing of engine-only rows | Out of lane; noted for root / A3 under Concerns. |
| Carried minors | The carry names none for AL-SIG. |

Contracts used:
- K-P9-1: two registry rows in the A1 schema. `formula_sha256` equals the C++ `formula_sha256(spec)`, and
  `ResearchFieldsAlSigFixture.CommittedRowsPinTheSpecs` checks it.
- K-P9-2: `BuilderKind` entries, with the stage pinned once per run in `SharedSources`.
- K-P9-11: `source_sample_end`, `predicted_mechanism` and `data_class` on every registration.

New schema literal (SQL2-CLS, for root's `classes.json`): `atx.p9-blocked-registration/v1`, used in the two
`scripts/specs/p9/blocked/*.json` files. The C++ reads two atx-db stage schemas but writes neither:
`atx.alpha-panel.thirteenf/v1` and `atx.alpha-panel.indexes/v1`.

## Evidence

**Fixture regeneration.** This was handoff step 1, after a docstring re-wrap in the generator. I ran
`python -B make_al_sig_fixture.py --out <scratch>` (exit 0, stdout `{"gia_13f": 2072, "russell_recon": 132}`). I
compared it byte for byte with `cmp` against the committed `atx-engine/tests/fixtures/research_fields_al_sig/`: all 29
files are identical and the file lists are equal. Scratch was deleted.

**Configure.** `powershell -NoProfile -File scripts\atx-build.ps1 configure -Preset equity-dev` in pool-13 exited 0:
`-- Configuring done (65.4s)` / `-- Build files have been written to: C:/atx-wt/pool-13/build-equity`.

**Build.** `powershell -NoProfile -File scripts\research-build.ps1 -Tag <tag> -Targets
"atx-engine-research-fields-tests,atx-research-fields"`:
- Tag `p9-alsig-a`: exit 1. Diagnostic: `russell_recon_field.cpp(168,43): error: use of undeclared identifier
  'iso_date'`. Fixed in `f1b3c792`.
- Tag `p9-alsig-b`: exit 0.
  - Receipt: `"ExitCode": 0, "CompiledTUs": 18, "Links": 4, "WallSeconds": 100.69, "Jobs": 4`.
  - Executables: `atx-engine-research-fields-tests.exe` sha256 `fcd568ba…24ca`, `atx-research-fields.exe` sha256
    `8fcb2bee…86be`.
  - The log has no compiler warning besides clang-cl's `/MP` unused-argument notice.
  - The build ran with the fix uncommitted (`DirtyEntries: 1`). That working tree's C++ equals `f1b3c792`, which is
    also HEAD's C++: `34e12223` touches only Python and JSON.

**gtests on `p9-alsig-b`.**
- `atx-engine-research-fields-tests.exe --gtest_filter=ResearchFieldsAlSig* --gtest_brief=1`: exit 0,
  `[==========] 24 tests from 4 test suites ran. (3768 ms total)` / `[  PASSED  ] 24 tests.`
- The whole target, no filter: `[  PASSED  ] 68 tests.` / `[  FAILED  ] 2 tests`.
  - The two failures are `ResearchFieldsWriter.QuantilesPartitionLikeNumpy` and
    `ResearchFieldsVolumeMean.SumOrderIsNumpys`. These are exactly the two pre-existing M1a-RED gtests that A3 fixes
    (`root-wave1-merge-report.md` (2)). They are not in AL-SIG's files.
  - Suites that passed, with test counts:
    - existing suites: Clock 7, Writer 7/8, VolumeMean 4/5, FinraAsof 5, Fixture 3, Cli 3, Registry 7, Manifest 8;
    - new AL-SIG suites: Thirteenf 8, Gia 6, Russell 5, AlSigFixture 5.

**pytest.** Each run used `"C:/Program Files/Python312/python.exe" -B -m pytest -q -p no:cacheprovider <paths>`:
- `atx-engine/tools/test_field_registry.py atx-engine/tools/test_no_new_python_builder.py`: exit 0, `39 passed in
  25.62s`.
- `scripts/tests/test_p9_al_sig_registrations.py scripts/tests/test_wave_queue.py`: exit 0, `13 passed in 9.18s`.
  Of these, `test_p9_al_sig_registrations.py` alone is `8 passed`.
- `ATX_RESEARCH_FIELDS_EXE=C:/atx-wt/pool-13/build-equity/bin/atx-research-fields.exe` with
  `atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` (`-rs`): exit 0, `11 passed in
  62.20s`, nothing skipped. This is the slice-3 flag-absent / engine-path identity with the lane's real executable.
- `python -B scripts/research_cycle.py candidates validate --dir scripts/specs/p9/candidates`: exit 0,
  `candidates: 1 valid in scripts/specs/p9/candidates`.

**ALSIG-1, WYZ: web lookup, no data opened.**
- The published RFS 2012 article (academic.oup.com) is paywalled, and its typeset text was not read.
- The authors' 2012 revision with the same title, *Forecasting stock returns through an efficient aggregation of mutual
  fund holdings* (CFR Working Paper No. 06-09 [rev.], 2012, EconStor hdl 10419/70129,
  `https://www.econstor.eu/bitstream/10419/70129/1/736379347.pdf`), states the following:
  - "We report the number of funds and the number of stocks that they hold, in aggregate, at the end of each year from
    1980 to 2006";
  - "Using the data during the entire sample period from 1980 to 2006";
  - "the first subperiod is 1980-1982 and the last subperiod is 2004-2006".
- The 2007 SSRN draft (abstract 891728, then titled *The Investment Value of Mutual Fund Portfolio Disclosure*) used
  1980-2002. The revision extended the sample to 2006.
- Result: `source_sample_end` = 2006 is verified.

**CHL check, same method.**
- NBER WP 19290 (August 2013, `https://www.nber.org/system/files/working_papers/w19290/w19290.pdf`) states "for the
  1996-2012 sample period" and "The sample period is from 1996 through 2012".
- Result: `russell_recon`'s `source_sample_end` 2012 is verified.

## How root verifies (after A3 merges, per ALSIG-FLIP)

1. Build: `powershell -File scripts\research-build.ps1 -Tag <p9-…> -Targets
   "atx-engine-research-fields-tests,atx-research-fields"`.
2. gtests:
   - `atx-engine-research-fields-tests.exe --gtest_filter=ResearchFieldsAlSig*:ResearchFieldsRegistry*` must give 31
     passed.
   - The whole `ResearchFields*` set must give 0 failed once A3's writer fix is in.
3. Flag-absent identity: `ATX_RESEARCH_FIELDS_EXE=<build>\bin\atx-research-fields.exe` with
   `pytest atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py`, run in its own session, not
   together with `atx-engine/tools`.
   - Also check `ResearchFieldsFixture.*` and `ResearchFieldsCli.*`.
   - The expected result: a v1 or v2 spec without `thirteenf` / `indexes` builds byte-identically. The three ported
     kinds are untouched.
4. K1 `--plan-only` of `rank(russell_recon)` (carry) on the fields build that will carry P9-S. `rank(gia_13f)` is no
   longer a registered string.
5. pytest: the four files above, plus `research_cycle.py candidates validate --dir scripts/specs/p9/candidates`.

## Decisions

- **Reader design.** The readers are split into a generic `PinnedStage` and typed pure decoders plus pure logic. This
  lets the logic be gtested without a parquet writer: atx-core cannot write DATE32, so the fixture parquet is written
  by pyarrow.
- **What each kind's entry pins.** Each kind's entry `sources` is only the stage manifest, which itself pins every
  stage file. So reuse follows the stage pin (`ReuseFollowsTheStagePin`).
- **Seal.**
  - Sealed holdings parts are never opened (the reader counts this).
  - A quarter whose V(P) is at or after the seal is not used.
  - Russell rows available at or after the seal are dropped (counted).
- **Russell index ids.** `r1000` and `r2000` are declared, not discovered. Any other spelling is refused, and the
  refusal lists the ids seen.
- **gia constants.** They were fixed blind in `GiaParams`: 8 / 6 quarters, 5 holders, 3 managers, half coverage, ratio
  screen 10, and the stage's 150-day staleness.
- **ALSIG-2 scope.** Only the DSL string is unregistered. The `gia_13f` field (kind and registry row) stays: it is a
  brief deliverable (DEC-5), the bar applies to strings (lit §3.0), and A4 reuses the 13F reader. If the PM wants the
  field row gone too, that is one removal commit (row, kind-table entry, fixture/registry tests).
- **Exclusion record.** The exclusion is recorded in this report and pinned by a pytest. I did not invent a new
  "excluded" file or schema.
- **No cell template** under `scripts/specs/p9/templates/`: the brief does not list one, and screening is root's P9-S.
- **Registry-less (v1) specs** also accept the two new kind names, because the kind table is the v1 name list.
  Existing v1 builds are unchanged.

## Concerns / questions for the PM

1. **ALSIG-3.** `prepare_research_fields_engine.ENGINE_FIELDS` does not route engine-only rows. `russell_recon` (and
   `gia_13f`) can therefore be built only by running `atx-research-fields build --registry` directly, with a v2 spec
   carrying `indexes` / `thirteenf`. Routing them through `prepare_research_fields.py --registry --engine-exe` is a
   root / A3 change.
2. **`hf_crowd` and the bar.** Lit 3.1 F2 puts its rho at .2, which is above the .1 bar, the same reason ALSIG-2
   excluded `gia_13f`. The brief names it explicitly as a `blocked_on_data` registration, so it stays, and its notes
   already say "above the .1 bar, check against inst_best_ideas before any screen". Should the ALSIG-2 logic remove it
   too, or should it stay blocked and be judged when the data lands?
3. **Merge order (ALSIG-FLIP).** AL-SIG merges after A3, and its fields are built only with A3's writer fix in. Until
   then the whole target shows the two M1a-RED reds.
4. **Lane build tree.** The `build-equity/` object tree in pool-13 was deleted after the run (DISK). The receipts and
   logs `build-equity/mega-p9-alsig-{a,b}-*` are kept as evidence. A fix round rebuilds in about 3 minutes with
   ccache: 65 s configure plus 100 s build.
5. **Cleanup slip in the shared scratchpad (my error).** Other agents in this session share the scratchpad
   (`C:\Users\natha\AppData\Local\Temp\claude\c--atx\0960fa49-…\scratchpad`). I cleaned my scratch there with
   `rm -f scratchpad/*.out scratchpad/*.log`.
   - That glob may also have deleted other agents' `.log` / `.out` files that were not open at that moment.
   - Three busy files survived: `c2-build-a.log`, `c2-configure-rel.log`, `d2-build-b.log`.
   - I cannot list what was removed.
   - The lanes' own `build-equity/mega-*` receipts and logs live in their pools and were not touched.
   - If any agent cites a scratchpad `.log` / `.out` as evidence, it should re-check that the file still exists.

## Deviations from brief

- Branch name: `feat/p9-al-sig-20261003` (the dispatch contract's form). The brief has `feat/p9-alsig-20261003`.
- Pool 13, not 20 (the carry's remap).
- `rank(gia_13f)` is not registered (ALSIG-2), so the brief's "strings for gia_13f and russell_recon" became
  `russell_recon` only.

## Ledger candidates

- WYZ 2012 (RFS 25(12)) sample is 1980-2006, from the 2012 CFR WP 06-09 rev. text; the 2007 SSRN draft used 1980-2002.
  CHL 2015 (RFS 28(1)) sample is 1996-2012, from NBER w19290.
- A reused pool builds `atx-engine-research-fields-tests` + `atx-research-fields` from cold in about 3 minutes with
  ccache warm (configure 65 s, build 100 s, 18 TUs). The lane's C++, written without a compiler, needed one fix (a
  missing `clock.hpp` include) to pass `/W4 /WX`.
