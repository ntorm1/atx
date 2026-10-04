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
  (compile fix: `russell_recon_field.cpp` includes `clock.hpp` for `iso_date`; the deletion of
  `scripts/specs/p9/candidates/gia_13f.json` was staged at that time and landed in this commit too), `34e12223`
  (ALSIG-2: the registration test pins `rank(gia_13f)` as excluded). The commit that adds this report comes after
  them. Everything is committed and the tree is clean. (Attribution corrected in fix round 1, review S5.) Fix round 1
  commits are listed in the "Fix round 1" section at the end.

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
- `scripts/specs/p9/candidates/gia_13f.json` was added in `2d7aba4e` and removed in `f1b3c792` (ALSIG-2; staged
  before the compile-fix commit), so the net diff from the base does not contain it.

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
| Ruling ALSIG-2: `gia_13f` not registered; record it as excluded by the bar | Done in `f1b3c792` (file removed) and `34e12223` (test). `scripts/specs/p9/candidates/gia_13f.json` is removed. `test_string_excluded_by_the_bar_is_not_registered[gia_13f]` pins that the id and `rank(gia_13f)` are in neither the queue nor the blocked set, and that the engine-only field row still exists. |
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
    also HEAD's C++: `34e12223` touches only the registration pytest.

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

## Fix round 1 (review `task-AL-SIG-review.md` at `1c649035`, CHANGES_REQUIRED)

Outcome: R1 fixed. I also did every cheap Suggested finding (S1, S2, S3, S5, S6). S4 is answered by ruling ALSIG-5.
I found and fixed one more defect while doing S1 (F1 below). The commits, on top of `1c649035`, are `39d8548e`,
`343efbc7`, `67401122` and `309507fb`. This report commit comes after them.

| finding / ruling | what was done | commit |
|---|---|---|
| **R1** (required): the stage pin was never checked on the `--reuse` path | `check_stage_pins(spec)` (`registry.hpp/.cpp`, cross-lane A2 file, appended at the tail) opens every stage the spec requests under its pin and contract, whether or not a planned field reads it. Both build paths (`build_fields`, `build_registry_fields` in `research_fields_cli.cpp`) call it twice: after `plan_fields` and before any reuse or build, and again after the last field, next to `repin_role`. A spec without `thirteenf` / `indexes` checks nothing, so the flag-absent path is unchanged. New gtest `ResearchFieldsAlSigFixture.ReuseRefusesAWrongStagePin`, below. | `39d8548e` |
| **F1** (found in this round, not in the review): wrong Russell index ids | The house indexes stage names its constituents index ids `R1000P`, `R2000P`, `R3000P`, `SP500P` (atx-db `docs/datasets/indexes.md` and `alpha_panel.indexes`, read from git history as documents and code; no data opened). `r1000` / `r2000` are only the membership file's flag columns. Effect of the old ids: on the real stage `russell_recon` would have been refused (no usable row), not silently wrong. Fix: `kRussell1000Id` / `kRussell2000Id` = `R1000P` / `R2000P`, and the spec text names them. | `343efbc7` |
| **S1**: `kIndexesStage` did not check its rule | The contract now requires `russell-proxy-v1`. The house manifest's `rule` is a list, `["russell-proxy-v1", "sp500-proxy-v1"]`, so the stage rule check (`stage.cpp`) accepts a string equal to the declared rule or a list that holds it. Anything else is refused, including a missing rule. New gtest `ResearchFieldsAlSigRussell.IndexesStageRule`. | `343efbc7` |
| F1 + S1, fixture | The generator now writes the house ids and the house rule list. I regenerated with the generator, not by hand. `expected/gia_13f.f64` and `expected/russell_recon.f64` are byte-identical to before. Only `indexes/constituents.parquet`, `indexes/manifest.json` and the `pins.indexes` value in `counts.json` moved. A second generator run into scratch was compared with `cmp`: 29 of 29 files identical. | `343efbc7` |
| F1, registry row | `russell_recon`'s spec text changed, so its `field_registry.json` row (`spec_text`, `formula_sha256` -> `c6c4b1aa…f1b8`) was re-derived from the C++ literals and written with `field_registry.dump`. It is still revision 1, because the field was never built. The `gia_13f` row is unchanged (`2e693083…e54f`). `CommittedRowsPinTheSpecs` passes, which confirms the C++ fingerprint matches the row. | `343efbc7` |
| **S2**: a null CUSIP could be mapped | `cusip_map_pit` rows with an empty CUSIP are skipped, and `screen_holding` never maps an empty holding CUSIP (house `build_13f`: null maps to unmapped). `RowScreen` gains the case. The fixture has no null CUSIP and is unchanged. | `67401122` |
| **S4 / ALSIG-5** | `russell_recon.json` notes now argue both halves of the lit 3.0 bar. The rho half is met: about 0 (F8). The SR_c half cannot be argued a priori: the trade is live about 30 sessions a year, only on the boundary crossers; the CHL post-2015 size is unverified (lit 2.3); the S&P addition effect is now below 1% [57]; and lit 6 puts the gain at +.00 to +.02. So the registration is kept but withdrawn from the trial queue: `proposed -> dropped`, no wave, applied through `wave_queue.transition`; `dropped` is final, so the string can never be pinned. Tests: `test_string_held_by_the_bar_is_out_of_the_trial_queue[russell_recon]` (pinning is refused, the history note names ALSIG-5, both halves are argued) and `test_the_lane_leaves_nothing_in_the_trial_queue`. | `309507fb` |
| **S3 / ALSIG-4** | `hf_crowd.json` notes record the ruling: it stays a blocked registration and is never queued without a new PM ruling on the bar. Pinned in `test_blocked_registration[hf_crowd]`. | `309507fb` |
| **ALSIG-6** | The `gia_13f` field and builder kind stay. `test_string_excluded_by_the_bar_is_not_registered` already pins that the row exists. | - |
| **S5**: report commit attribution | Corrected above. The deletion of `gia_13f.json` landed in `f1b3c792`, because it was staged before that commit. History was not rewritten. | report |
| **S6**: no evidence for `lazy_prices` / `hf_crowd` `source_sample_end` | Lazy Prices (Cohen, Malloy and Nguyen 2020, JF 75(3)): sample 1995-2014, so 2014. BHL (Brown, Howard and Lundblad 2022, RFS 35(7)): about 1,500 hedge funds 2004-2016, so 2016. Both come from lit 3.1 F1 / F2 ([web]) and were re-verified against the papers by the reviewer (review "Citations"). I did no new lookup. | report |

New gtest `ResearchFieldsAlSigFixture.ReuseRefusesAWrongStagePin`:
- Setup: a valid reuse directory and unchanged stages.
- Refused cases: a wrong `thirteenf` pin, a wrong `indexes` pin, and a wrong pin on a requested stage that no planned
  field reads. Each wrong pin is a real digest of the other stage.
- Each refusal carries "manifest SHA-256 does not match the pin", the output directory stays empty, and no receipt is
  written.
- Control: the right pins with the same reuse directory reuse both fields.

Cross-lane edits added in this round (all A2 files, minimal):
- `registry.hpp`: one declaration appended at the tail.
- `registry.cpp`: two includes and one function appended at the tail.
- `research_fields_cli.cpp`: three `check_stage_pins` call lines, one per call site.

There are no new files and no CMake change.

### Evidence (fix round 1)

**Configure.** `powershell -NoProfile -File scripts\atx-build.ps1 configure -Preset equity-dev` exited 0:
`-- Configuring done (41.7s)` / `-- Build files have been written to: C:/atx-wt/pool-13/build-equity`.

**Build.**
- Command: `powershell -NoProfile -File scripts\research-build.ps1 -Tag p9-alsig-c -Targets
  "atx-engine-research-fields-tests,atx-research-fields"`, exit 0.
- Gate: admitted at 1846 MiB free, `Jobs` 2. pool-15's Release build was running at the same time.
- Receipt: `"ExitCode": 0, "CompiledTUs": 94, "Links": 8, "WallSeconds": 141.19`. The tree was cold after the
  round-0 deletion.
- Executables: `atx-engine-research-fields-tests` `bb646215…73d6`, `atx-research-fields` `2a78cf2c…0b0a`.
- Warnings: only the two third-party databento `getenv` deprecation warnings and clang-cl's `/MP` notice.
- Source: the build ran on the working tree over `1c649035` with every fix-round C++ edit in place (`DirtyEntries`
  21). Its C++ equals the committed C++ of `309507fb`: no C++ file was edited after the build started.

**gtests.**
- `atx-engine-research-fields-tests.exe --gtest_filter=ResearchFieldsAlSig*:ResearchFieldsRegistry* --gtest_brief=1`:
  exit 0, `[==========] 33 tests from 5 test suites ran.` / `[  PASSED  ] 33 tests.`
  - AL-SIG accounts for 26 of them: Thirteenf 8, Gia 6, Russell 6, Fixture 6. Registry accounts for 7.
- The whole target: `[==========] 72 tests from 12 test suites ran.` / `[  PASSED  ] 70 tests.` /
  `[  FAILED  ] 2 tests`.
  - The two failures are the pre-existing M1a-RED pair that A3 owns (`ResearchFieldsWriter.QuantilesPartitionLikeNumpy`,
    `ResearchFieldsVolumeMean.SumOrderIsNumpys`).
  - Every other suite passes, including Fixture 3/3, Cli 3/3 and Manifest 8/8.

**pytest.** Each run used `"C:/Program Files/Python312/python.exe" -B -m pytest -q -p no:cacheprovider`:
- `scripts/tests/test_p9_al_sig_registrations.py scripts/tests/test_wave_queue.py`: exit 0, `15 passed in 10.11s`.
- `atx-engine/tools/test_field_registry.py atx-engine/tools/test_no_new_python_builder.py`: exit 0,
  `39 passed in 40.46s`.
- `ATX_RESEARCH_FIELDS_EXE=C:/atx-wt/pool-13/build-equity/bin/atx-research-fields.exe` with
  `atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` (`-rs`): exit 0, `11 passed in
  10.39s`, nothing skipped. This is the flag-absent identity check with the real executable.
- `scripts/research_cycle.py candidates validate --dir scripts/specs/p9/candidates`: exit 0,
  `candidates: 1 valid in scripts/specs/p9/candidates`.

### For root (updates to "How root verifies")

- gtest filter `ResearchFieldsAlSig*:ResearchFieldsRegistry*` must now give **33** passed (was 31).
- K1 `--plan-only` of `rank(russell_recon)` stays a root item (ALSIG-5), even though the string is now withdrawn from
  the trial queue.
- Field changes:
  - The Russell reader now expects the house ids `R1000P` / `R2000P`.
  - It also expects a manifest `rule` list holding `russell-proxy-v1`.
  - The first real `russell_recon` build is the check on the real stage; the docs say this is its shape.
- Disk: after these runs the `build-equity/` object tree was deleted again. Only my own entries were removed, by name.
  The `mega-p9-alsig-{a,b,c}-*` receipts and logs are kept, and so are the pre-existing `audits/` and
  `v8-interim3-pitch-render-run2/`.
- Scratch: my two fix-round scratch scripts and the configure and build output files in the shared scratchpad were
  deleted by exact name, with no wildcard.
