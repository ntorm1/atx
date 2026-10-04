# P9 pre-flight conflict scan (read-only)

Scanner: read-only pre-flight agent, 2026-10-03. Tree: `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, head
`d7c1c520`. Nothing built, nothing run, no data or `build-*` output opened, `atx-db/` not touched, this file the only
write.

Inputs read in full:
- `docs/plans/2026-10-03-p9-sprint-plan.md` (the plan, cited "plan §n").
- `docs/plans/2026-10-03-p9-lane-briefs.md` (the briefs). The split copies in `briefs/brief-<ID>.md` were checked
  line by line: every master line of each lane section is present in its split file (22/22, 0 missing).
- `docs/plans/2026-10-02-p9-code-review-dsl-ic.md` (DS review).
- Targeted reads of the orchestration, NAV, composition and core-audit reviews where a row cites them.

DS review drift check: the file is untracked (no git history). Its mtime is 2026-10-02 23:08, before the plan
(23:26:40) and the briefs (23:26:22). No post-plan edit is detectable.

Key: OK = consistent. "OK w/ caveat" = consistent if the stated condition holds. CONFLICT = the two texts cannot both
be satisfied as written. "PM #n" = item n of the ruling list at the end.

---

## 1. Lane pairs that share a file or an interface

### 1a. Wave 1 (E1, A1, A2, B1, C1, D1, S1, T1)

| # | lane X | lane Y | X produces vs Y consumes, or the shared file | finding |
|---|---|---|---|---|
| 1 | E1 | S1 | Shared `scripts/research_cycle.py`. E1 task 0 edits :369-376, :1015-1019 and :1176. S1 adds `--candidates` to `MARGINAL_SPEC_FLAGS` (:211) as a "one-line" cross-lane edit. | CONFLICT: the validator (:528-532) accepts digit values only, so `--candidates ID,...` would be refused. Plan §2.4 S1 says "E1 owns the wave call site", but E1's brief has no `--candidates` task, and E2's brief says "(if S1 did not)". Three owners, none complete (PM #4). |
| 2 | E1 | T1 | Shared dir `scripts/tests/` (different files). T1 owns `fixtures/tiny_world.py` and `test_cycle_e2e.py`. E1's root check ("a tiny-world wave end to end") uses T1's fixture. | OK w/ caveat: E1 (slot 1) changes the run-dir / attempt layout (K-P9-10, attempt sub-dirs). T1 (slot 2) must re-run its pytest on E1's merged tree, and its canary should pin payload SHAs, not run-dir paths. |
| 3 | E1 | C1 | E1 task 0 (c): a NAV phase with `--capacity-curve` is done only when `summary.json`, `capacity_curve.csv` and `v7_extras.json` exist in `<out>/`. C1 writes `summary.json` last and moves capacity into the main lockstep (today the capacity sub-run writes `<out>/capacity/`, v7:1127). | OK w/ caveat: C1 must keep those three names at `<out>/` (curve written at v7:393) or list the new layout in its byte-identical list. The exe identity in E1's receipts (`executable_sha256`) and in C1's recipe (git SHA, build type) are compatible. |
| 4 | E1 | B1 | B1 writes the PM7-35 sign rule as one C++ predicate, "both written, the PM picks one before merge". The Python twin is `scripts/wave_rules.py:50-51`, which E1 owns in wave 1. | CONFLICT (gap): if the PM picks the gate rule (runner sign = prior), `wave_rules.py` must change, and no brief assigns that edit. Either way the Python copy becomes a G-P5 mirror (PM #9). |
| 5 | A1 | A2 | `atx-engine/tools/prepare_research_fields_engine.py`. A1 owns `prepare_research_fields*.py` (plan §2.2 glob) and its deliverable (6) fixes `engine.py:13`. A2's brief lists the same file in scope ("becomes a registry kind: engine caller"). | CONFLICT: two owners of one file in one wave (PM #5). |
| 6 | A1 | A2 | K-P9-1 `field_registry.json` (A1 creates it). Plan §2.4 A2: "the engine shim ... becomes a registry `kind: engine` row". | CONFLICT: A2 cannot edit A1's JSON. If A1 writes `kind: engine`, A1's own root check (v15 rebuilt through the entry) needs A2's exe, which merges one slot later. The `builder` value of an engine row (BuilderKind id or field name) is not defined (PM #5). |
| 7 | A1 | A2 | Rename `rows_available_on_or_after_2025_dropped` to `rows_sealed_dropped`: Python is A1's (prep:948, 1689, 1754), C++ is A2's (`research_fields_cli.cpp:116`). The key also appears in `tests/fixtures/research_fields/expected/manifest.normalized.json`, `test_research_fields_fixture.py`, `tests/research/research_fields_fixture_test.cpp:197`, `prepare_recent_research.py:863` (role builder) and `atx-impl/tests/fixtures/lo1_label_role_pair/*/manifest.json`. | CONFLICT: A1's fixture scope is "Python generator only", A2's test path does not exist, and nobody owns `expected/` or the fixture pytest. A2's "existing fixture identity unchanged" cannot hold after the rename. The role-builder key stays unrenamed, giving two names for one count (PM #5). |
| 8 | A1 | A2 | K-P9-3: A2 writes `producer: {kind, exe_sha256, git_sha, build_type, receipt_sha256}`, and A1 is named as the reader ("manifest writer"). Today's entries already carry `producer: {module, code_sha256, code_sha256_lf, code_git_blob_sha1}`, which reuse reads (prep:2795, 2853). | CONFLICT: same key, different shape, and A1's brief never mentions K-P9-3. Who writes the final manifest of a mixed python + engine build (A1's entry or A2's publish-last exe) is not stated (PM #6). |
| 9 | A1 | A2 | Reuse key and seal refusal: A1 in Python (`load_prior`, readers, version keys), A2 in C++ (`reuse.{hpp,cpp}`, seal refusal on a prior manifest). | OK w/ caveat: one rule in two languages in the same wave (table 5 #5). Needs a G-P5 allowlist row with an expiry lane. |
| 10 | A1 | T1 | A1: field readers refuse `seal.exclusive_end` != the research seal. T1's `tiny_world.py` writes a fields manifest with no `seal` block (:277-289). | CONFLICT (minor): if an absent seal counts as a mismatch, the tiny-world canary and tests break. G-P9's C++ consumer half (IC exe, NAV) is assigned to no lane (PM #13). |
| 11 | A2 | T1 | `atx-engine/tests/CMakeLists.txt:368-381`. A2's new gtests (`ResearchFieldsRegistry.*`, `ResearchFieldsManifest.*`) must join the explicit `atx-engine-research-fields-tests` source list. T1 adds that target's CTest / label lines in the same block. A2's brief forbids only "CTest lines". | CONFLICT: same block, two lanes. A2 avoids it only by adding cases to already-listed TUs (PM #2). |
| 12 | A2 | B1 | `atx-engine/CMakeLists.txt`: A2 edits the fields block (really 248-282, sources at 252-265); B1 appends a new admission-library block. | OK under the "append one block" rule (root resolves). |
| 13 | B1 | T1 | B1's tests: `FactorsVerb.*` (an atx-impl test, so the explicit `atx-impl-strategy-target-tests` list or a new target) and `ResearchAdmission.*` (needs a target linking the new admission library). Both CMake files are T1's. | CONFLICT: B1 cannot register its tests without editing T1's files. T1, coding at base, cannot register B1's new test exe, so G-P4 ("every research test target") is incomplete after wave 1 (PM #2). |
| 14 | C1 | T1 | `atx-impl/tests/CMakeLists.txt:114-134`: new C1 test TUs must be listed in `atx-impl-strategy-target-tests`. Otherwise a new `*_test.cpp` is globbed only into `atx-impl-tests` (:40-42), which is not root's named target. | CONFLICT unless C1 puts its cases in already-listed files (`strategy_nav_replay_test.cpp`, `strategy_live_test.cpp`, `strategy_vol_target_test.cpp`, `strategy_cost_v2_test.cpp`). Also, T1's canary NAV golden is re-pinned after C1 (slot 8) moves summary keys, so it belongs in C1's ruled substitution list (PM #2). |
| 15 | D1 | T1 | The `atx-impl-strategy-ic-tests` explicit list (:95-112) must carry `CompositionRules.*` / `IcAdmission.*`. | CONFLICT unless D1 puts the cases in `strategy_ic_composition_test.cpp` / `strategy_ic_runner_test.cpp` (PM #2). |
| 16 | S1 | T1 | `MarginalIc.*` and `IcIdentity.*` fit existing listed TUs (`strategy_marginal_ic_test.cpp`, `strategy_ic_runner_test.cpp`). Release builds use `research-build.ps1 -Preset equity-rel`, which `ValidateSet` (:44) already accepts. | OK. |
| 17 | B1 | D1 | `atx-impl/tools/fit_composition_weights.py`: D1 edits the dispatch and the theme tuple (`V7_APPENDED_THEMES` reads `registry.json`). B1's comparator pytest imports `screen_v4` (:1613) and `factor_record` (:1160) read-only. | OK w/ caveat: D1 must keep both signatures, and the import-time registry read must not break B1's synthetic import. |
| 18 | B1 | D1 | `atx-impl/CMakeLists.txt`: B1 lists "the atx-impl CMake source line" (`strategy_factors_verb.cpp`, pattern at :205). D1 adds `strategy_ic_rules.cpp`, but its scope (plan §2.2 D1) omits the file. D1 probably also needs the per-TU /O2 lists (:115-124). | CONFLICT (minor, scope): D1 needs a listed append-only edit (PM #2). |
| 19 | D1 | S1 | D1 deliverable: "one centred-tied-rank helper (3 C++ copies)". CM §2 names `strategy_ic_composition.cpp:31-41` (D1's), `marginal_rank_ic.cpp:98-129` (S1-owned) and engine `combine/group_rerank.hpp:44` (unowned). | CONFLICT: unifying all three edits S1's file in the same wave (PM #8). |
| 20 | D1 | S1 | `strategy_ic_detail.hpp`: S1 replaces its copied `hash_valid` / `safe_id` / `pinned_json` (marginal:52-92) with "the shared ones", which live in D1's detail header. | OK w/ caveat: S1 only includes the header, so D1 must keep those helper signatures. |
| 21 | D1 | S1 | Same exe `atx-equity-strategy-ic`: D1 edits runner / admission / composition TUs, S1 the marginal verb, caches and preset. | OK: disjoint TUs. The marginal verb takes themes from the weights file, not from D1's new theme argument. |
| 22 | C1 | S1 | Both use the `equity-rel` preset (CMakePresets.json:48-55; it already exists): S1 to adopt Release IC, C1 to compare Release NAV with Debug. | OK. S1's "Release equity preset target" deliverable is already present at head. |
| 23 | E1 | S1 / C1 | K-P9-10 asks for `build_type` in every bounded receipt. Possible sources: S1's build token in `vm_identity` / `ic_identity`, C1's build type in the NAV recipe, or the spec `build` key -> `BUILDS` (`research_cycle.py:223-227`, which already has `equity-rel`). | OK w/ caveat: the natural source is the spec build key, but it lives in `research_cycle.py`, outside E1's post-task-0 scope (PM #3). |

### 1b. Wave 2 (E2, A3, S2, B2, D2, C2, AL-COMB, AL-SIG)

| # | lane X | lane Y | X produces vs Y consumes, or the shared file | finding |
|---|---|---|---|---|
| 1 | A3 | AL-SIG | K-P9-2 source: A3's `sources/vendor_panel` reads TickerHistory3 only. AL-SIG's `gia_13f` needs 13F holdings plus the managers' past holding returns; `russell_recon` needs the `indexes/` Russell proxy (lit F8 row: "missing: field builder research_fields_indexes.py"). | CONFLICT: neither reader is in K-P9-2, and the C++ 13F reader is A4's wave-3 deliverable (PM #18). |
| 2 | A3 | AL-SIG | Shared append points: kind rows in A2's `research/fields/registry.cpp`, rows in `field_registry.json`, and TUs in the fields library source list (atx-engine/CMakeLists.txt:252-265). | OK under the append rule (A3 merges slot 2, AL-SIG slot 8). |
| 3 | A3 | E2 | E2's `field` kind builds a fields dir from the registry rows (K-P9-1) that A3 appends. | OK. |
| 4 | B2 | E2 | E2's `research_common.py` absorbs the sha256 helpers x6, including `backtest_integrity.py:563`, and "ledger readers x3". B2 owns `backtest_integrity.py` and `scripts/research_ledger.py` in wave 2. | CONFLICT: E2's consolidation edits files B2 owns (PM #14). |
| 5 | B2 | E2 | `scripts/wave_stage_record.py` (B2's listed edit: write the ledger head) vs E2's run-dir / receipt-helper consolidation (`wave_context.py`, `wave_stage_util.py`, `cycle_verdict.py`). | OK w/ caveat: E2 must leave `wave_stage_record.py` alone. |
| 6 | B2 | C2 | `atx-impl/CMakeLists.txt` (C2's in wave 2 per §2.2): B2 moves `trial_ledger.cpp` down into the engine ledger library and leaves a thin include. | CONFLICT (minor): B2 edits C2's file, and B2's brief does not list `atx-impl/src/trial_ledger.*`. |
| 7 | B2 | D2 | `atx-engine/CMakeLists.txt` and `atx-engine/tests/CMakeLists.txt`: both add libraries, exes and test targets (`atx-research-eval`, `atx-research-composition`). | OK under the append rule (the tests CMake file has no wave-2 owner). |
| 8 | C2 | E2 | C2's listed edit makes the match stage (`wave_stage_cell.py:92-130`) call `--calibrate-gross` unconditionally. E2's acceptance: every v8 manifest plans byte-identical argv. | CONFLICT (PM #15). |
| 9 | C2 | D2 | `atx-impl/CMakeLists.txt`: D2's relocation turns `strategy_ic_{shrink,theme_resid,theme_erc,theme_tsmom,two_speed}` into thin includes and links the new engine library. | CONFLICT (minor): D2 edits C2's file and atx-impl rule TUs that are not in D2's §2.2 row (PM #17). |
| 10 | C2 | AL-COMB | `atx-impl/CMakeLists.txt`: AL-COMB adds the TU `strategy_ic_theme_hedge.cpp`. | OK under the append rule (listed cross-lane edit). |
| 11 | D2 | AL-COMB | D2 relocates the `strategy_ic_*` rules, theme-tsmom included, into engine `research/composition`. AL-COMB adds `strategy_ic_theme_hedge.*` in atx-impl, a row in D1's `strategy_ic_rules.cpp`, and a `mom-volman` schedule rule "beside theme-tsmom". A new rule's weights block also needs a fitter plugin entry, and D2 owns the fitter. | CONFLICT (PM #17). |
| 12 | D2 | E2 | E2 validates `rules:` against the K-P9-6 list-rules JSON, which D2's relocation must keep stable. | OK w/ caveat: D2 must keep `--list-rules --json` byte-identical (pin it). |
| 13 | E2 | S2 | E2 drops the `--help` probe (`exe_capabilities`, research_cycle.py:676-687: "no-composition", "marginal"). S2 adds IC flags (`--label-terminal`, `--eval-mode audit-exact`, HAC rule). | OK w/ caveat: works only if the K-P9-6 envelope carries capabilities (PM #7). |
| 14 | E2 | AL-COMB | E2's K-P9-9 `rules: [{name, params}]` vs AL-COMB's templates `scripts/specs/p9/templates/*.json` "with rules: blocks". | CONFLICT (minor): K-P9-9 says `name` while K-P9-6 / -7 say `id`, and AL-COMB writes its templates before E2 merges. |
| 15 | E2 | AL-SIG | Separate dirs: `scripts/specs/p9/kinds/` (E2) and `scripts/specs/p9/candidates/` (AL-*). | OK. |
| 16 | S2 | D2 | S2 owns alpha / factory and is forbidden composition; D2 does not touch alpha. | OK. |
| 17 | AL-COMB | AL-SIG | Both write registration files into `scripts/specs/p9/candidates/` (distinct ids), and both need the K-P9-11 schema. | OK (see the K-P9-11 row in 1d). |
| 18 | A3 | B2 / D2 | `atx-engine/CMakeLists.txt` appends. | OK. |

### 1c. Wave 3 (C3, D3, AL-CLOCK 3b, AL-DATA, PRE, A4)

| # | lane X | lane Y | X produces vs Y consumes, or the shared file | finding |
|---|---|---|---|---|
| 1 | C3 | D3 | `atx-impl/CMakeLists.txt`: C3 task 3 splits `atx-impl-core` into `atx-impl-strategy` / `atx-impl-pipeline`. D3 edits runner TUs listed there (theme schedule `strategy_ic_composition.cpp:409-420`; weights parse in `strategy_ic_admission.cpp`). | CONFLICT (minor): D3 rebases onto C3 (slot 1, then 2). D3's brief has no Files-in-scope line, and its runner TUs are not in §2.2's D3 row. |
| 2 | C3 | AL-CLOCK | K-P9-7 trade-rate registry (C3 writes it, AL-CLOCK adds an entry). The per-name rate in aim-partial-v5 lives in `strategy_target_replay.cpp:345-399`. | OK: sequential (3b after C3). Caveat: AL-CLOCK has no scope line, and `strategy_target_replay.cpp` is in no lane's scope (C1 forbade it beyond call sites). |
| 3 | C3 | AL-DATA | AL-DATA's implied-borrow "name-level fee path into `book/borrow_schedule.hpp`" reaches the NAV cost path that C3 is turning into registries. | CONFLICT (conditional): only if the OD-P9-4 / -5 data lands; otherwise AL-DATA is design-only. |
| 4 | D3 | E2 (merged) | D3 wires the `walk-forward` kind into E2's `scripts/cycle/*` and `wave_manifest.py`. | OK w/ caveat: those files have no wave-3 owner, and D3 lists no scope. |
| 5 | A4 | AL-DATA | Form ADV crowding joins on the 13F filer CIK, so it needs a 13F reader, which A4 builds in C++ in the same wave. | CONFLICT (conditional on Form ADV landing). |
| 6 | PRE | AL-CLOCK | Plan §3.2 makes AL-CLOCK's report a hard need of PRE (DAG edge ALK -> PRE), but AL-CLOCK is 3b (after C3 merges) while PRE is dispatched in wave 3. §2.1 lists PRE's needs without AL-CLOCK. | CONFLICT (ordering; PM #1). |
| 7 | PRE | AL-DATA, B2, E2 | PRE reads AL-DATA reports (soft), K-P9-5 (`house_v1`, the conditional print) and K-P9-9. | OK. |
| 8 | A4 | C3 / D3 | No shared file. | OK. |

### 1d. Cross-wave K-P9 producer -> consumer (writer brief text vs reader brief text)

| contract | writer -> reader | do field names, CLI flags and schema ids agree? | finding |
|---|---|---|---|
| K-P9-1 | A1 -> A2 | Row keys `kind` / `builder`: A2 "reading K-P9-1" and registering kinds `vol_126`, `si_shares`, `si_dtc`. Whether `builder` holds a BuilderKind id or a field name is undefined, and nobody is assigned to write the engine rows. | CONFLICT (PM #5). |
| K-P9-1 | A1 -> A3 | "registry rows `kind: engine` ... added to A1's JSON". | OK. |
| K-P9-1 | A1 -> AL-SIG | "registry rows". | OK. |
| K-P9-1 | A1 -> E2 | The `field` kind builds from rows. The declared `dtype` has no consumer: the IC typecheck still infers Group from the `grp_` prefix (DS §2). | OK w/ caveat (table 4 #10). |
| K-P9-2 | A2 -> A3 | `BuilderKind {id, parse, build}` matches. "Shared through the build context": A2's brief defines no context slot for sources. | OK w/ caveat. |
| K-P9-2 | A2 / A3 -> AL-SIG | AL-SIG's readers (13F, `indexes/`) are not in K-P9-2 (vendor_panel only). | CONFLICT (PM #18). |
| K-P9-2 | A3 -> AL-DATA, A4 | "on A3's sources / source interface". | OK. |
| K-P9-3 | A2 -> A1 | A1's brief is silent, and the `producer` object already exists with other keys. | CONFLICT (PM #6). |
| K-P9-3 | A2 -> E2 | E2's Read line lists K-P9-1, -6, -7, -9, -10, not -3. | OK (minor gap). |
| K-P9-4 | B1 -> D2 | "from the factor series". `factor.f64` / `tau.f64` / `manifest.json` are not restated but not contradicted. Tolerance (0, else 1e-12) is the same in plan and brief. | OK. |
| K-P9-4 | B1 -> D3 | "from cached factor series". | OK. |
| K-P9-5 | T1 (fixture) -> B2 | T1's gtest must reproduce paired CBB dSR, Memmel SE and house DSR "from the engine headers", but these exist only in Python at base. CBB, ONC and PBO draw from numpy's RNG. | CONFLICT (PM #11). |
| K-P9-5 | B2 -> `nav_summ.py`, PRE | `--engine-stats` is the same in plan and brief; `house_v1` and the conditional print match PRE's Read line; JSON keys are guarded by `EvalVerb.JsonSchema`. | OK. |
| K-P9-6 | D1 -> E2 | E2 needs exe capabilities from list-rules, but K-P9-6 is a bare rules array, and D1's struct lacks `version`, `params_schema` and `incompatible[]`. | CONFLICT (PM #7). |
| K-P9-6 | D1 -> AL-COMB | "one row in D1's table" matches. The fitter plugin for the rule's block is unassigned (PM #17). | OK w/ caveat. |
| K-P9-6 | D1 -> D2, D3 | Consistent. | OK. |
| K-P9-7 | C1 -> E2 | Leverage rows via `nav --list-rules --json`. The envelope follows the K-P9-6 ruling. | OK w/ caveat. |
| K-P9-7 | C3 -> AL-CLOCK | Trade-rate entry. Rows of `kind: target` are assigned to no lane. | OK (minor gap). |
| K-P9-8 | D3 -> runner, E2 | `atx.composition-weights/v2` vs the existing `atx.dsl-composition-weights/v1` and `/v2` (v2 = theme block; `strategy_ic_admission.cpp:22-25`). | CONFLICT (PM #19). |
| K-P9-9 | E2 -> PRE, root | The kind list matches the plan (add, replace, rule, field, role, horizon, walk-forward, leverage). How G-P2's 10 kinds map onto these 8 manifest kinds is not stated. v2 must be added in `wave_manifest.py` (v1 at :60), a file E2 does not own. | CONFLICT (PM #14). |
| K-P9-9 | E2 -> AL-COMB / AL-CLOCK templates | `rules[].name` vs K-P9-6 `id`. | CONFLICT (minor). |
| K-P9-10 | E1 -> E2, scoreboard | Keys `argv_sha256`, `attempt`, `executable_sha256`, `build_type` agree. The `build_type` source sits in `research_cycle.py` BUILDS. | OK w/ caveat (PM #3). |
| K-P9-11 | E1 -> AL-*, PRE | Keys agree: `source_sample_end` (YYYY), `predicted_mechanism`, `data_class` (H / W / P / N). `wave_queue.py` defines only `atx.wave-candidate/v1`; AL-COMB's and AL-CLOCK's "rule registration files" have no schema owner. The default dir is `scripts/specs/v8/candidates/`; `--dir` exists, so `p9/candidates/` works. | CONFLICT (minor: no schema for rule files). |

---

## 2. Self-consistency of each lane brief

Each row asks four things. Tests: do the tests it names match the code it asks for? Files: are the files it creates
inside its scope? Plan: do its needs, pool, wave and effort match plan §2.1 / §3.2? Checkable: can a lane that
cannot build C++ or run real data check its acceptance?

| lane | tests vs code | files vs scope | needs / pool / wave / effort | acceptance checkable by the lane | finding |
|---|---|---|---|---|---|
| E1 | 11 named tests; task 0 (a)-(f) each has one. Untested: host memory semaphore, `lock` writing `exes_sha256`, queue-history date, reader-code-SHA reuse, per-run `cycle_verdict.json`, K-P9-11 keys, compiler-process wait. | OR-2 (`lock()` :1793, :543-552), OR-3 resume binding for u / fit / w / card / marginal (:883-911, 1071-1077, 1323-1332) and OR-4's hard stop (:906-910, 1646-1647) are all in `research_cycle.py`, which the scope allows for "task 0 only". Plan §2.4 asks E1 for "`candidates pin --by` checked against a role list" and the `--candidates` wave call site; the brief has neither. Attempt naming `<output>/attempt-k/` vs today's `-run<k>` / `-<k>` and OR-4's "`-run<k>`" is unresolved. | 17 / 1 / M / slot 1: matches. | Yes (pytest on fakes); root runs tiny-world. | CONFLICT (PM #3, #4) |
| A1 | Names `test_no_new_python_builder.py`, a v15-order test and a PRODUCERS / IMPORTS test (none in a Tests line). Untested: version keys, seal refusal, the key rename. | Deliverable (6) edits `engine.py` (in A2's scope). The rename forces `expected/manifest.normalized.json` and `test_research_fields_fixture.py`, but the scope is "Python generator only". Removing the conftest bind regenerates fixtures in about 25 tool test modules (conftest docstring), yet the scope says only "test_* for these", and effort M looks low. The K-P9-3 reader role is absent. | 12 / 1 / M / slot 3: matches. | Yes (pytest `atx-engine/tools`); root rebuilds v15. | CONFLICT (PM #5, #6, #13) |
| A2 | `ResearchFieldsRegistry.*`, `ResearchFieldsManifest.*` and fixture identity match the deliverables. "Existing fixture identity unchanged" contradicts the key rename it is told to make. | Scope path `atx-engine/tests/research_fields/**` is missing; the real tests are `atx-engine/tests/research/research_fields_*_test.cpp`, listed in T1's tests CMake at :368-374. CMake block cited :266-282, real 248-282 (sources at 252-265). `engine.py` is shared with A1. Fixture `expected/` is not in scope. | K-P9-1 soft; 13 / 1 / L / slot 4: matches. | Root-only (expected for C++). | CONFLICT (PM #2, #5) |
| B1 | Six gtests match the deliverables. The comparator pytest "checks the C++ outputs' committed fixture bytes", but the lane has no C++ outputs, so the fixture must be fitter-generated. | New admission library and its CMake block are in scope. Its tests need T1's CMake files. The PM7-35 Python side is unassigned. `hac.hpp` needs no edit: BartlettV1 with `small_sample_correction=false` already gives the fitter's /n (fit:1572-1583). | 14 / 1 / L / slot 6: matches. | Root: "admission.csv byte for byte". t-stats from numpy `e @ e` (BLAS order) vs a C++ sequential sum likely differ in the last bit, and K-P9-4's 1e-12 escape does not cover admission.csv. | CONFLICT (PM #2, #9, #12) |
| C1 | Six suites cover deliverables (1)-(6). No test for the `nav --list-rules --json` leverage rows. | In scope. New test TUs need T1's list unless the cases go in listed files. `sqrt` is applied only at `replay_cost.cpp:92`; the brief keeps `pow` at `strategy_cost_v2.cpp:45,138,196,231`, but :45 is `std::cbrt` and :138 / :196 raise to `s2.impact_delta` = .5, the only registered value (NV §3 lists them as non-exact). | 15 / 1 / L / slot 8: matches. | Yes for the sqrt-path bits (Python's sqrt is correctly rounded); root does the Release compare, which C1's own `pow` leftovers may fail on capacity / v7 cells. | CONFLICT (PM #2, #10) |
| D1 | Five gtests plus pytest. The struct `{id, block_key, parse, verify, apply_stage, recipe_text, working_bytes}` lacks K-P9-6's `version`, `params_schema` and `incompatible[]`, yet `.IncompatiblePairRefused` needs `incompatible`. | New `strategy_ic_rules.{hpp,cpp}` needs `atx-impl/CMakeLists.txt` (not in scope). The rank helper touches S1's `marginal_rank_ic.cpp` and the unowned `group_rerank.hpp`. `composition_rules.py` in scope is an existing 390-line ew-theme-std mirror inside D2's 1,535-line deletion set, so the plugin list must not live there. | 16 / 1 / M / slot 7: matches. | Yes (pytest); root runs X-5 identity. | CONFLICT (PM #2, #7, #8) |
| S1 | Six gtests. No test for `--exclude-self` or the timers. | `ic_identity` "from ic_screen.cpp's TU" needs a new accessor in `atx-engine/src/factory/ic_screen.cpp` and `factory/ic_screen.hpp`, outside S1's row (today only `ic_screen_simd_width()` exists, result_cache.cpp:45-48). The "Release equity preset target" already exists (`equity-rel`, CMakePresets.json:48-55). The one-line `MARGINAL_SPEC_FLAGS` edit is invalid. | 18 / 1 / M / slot 5: matches. | Root. | CONFLICT (PM #4); scope gap (ic_screen accessor) |
| T1 | The tie gtest "reproduces each from the engine headers", but paired CBB dSR, Memmel SE and house DSR have no engine implementation at base (no memmel / cbb / paired in `eval/`), so it cannot be green at merge. CBB (nav_summ.py:336-341), ONC (k-means, backtest_integrity.py:424-430) and PBO (seeded) use numpy Generator draws. | The strategy exes are already CTest-labelled (atx-impl/tests/CMakeLists.txt:149-154). Only fields-tests and the new wave-1 targets remain, and the latter do not exist at T1's base. Class-C files live only in `atx-impl/strategies/`, which no other lane owns. | 19 / 1 / M / slot 2: matches. | Pytest guards yes; canary and tie gtest root. | CONFLICT (PM #2, #11) |
| A3 | `VendorPanel.HashOnce`, `.SealPushDown`, `FactorBreak.ClosedForm`, leak probes and fixture identity vs the Python builders: all checkable through Python. | In scope, including rows in A1's JSON (listed). `arrow[parquet]` is in vcpkg.json:5 and atx-core links it. | A2 merged; 12 / 2 / L / slot 2: matches. | Yes (Python fixtures); root does TRAIN identity. | OK |
| B2 | Matches the deliverables. | Plan row plus listed `wave_stage_record.py`. Also needs `atx-impl/src/trial_ledger.{hpp,cpp}` (thin include) and `atx-impl/CMakeLists.txt` (C2's), neither listed. | T1 merged; 14 / 2 / L / slot 4: matches. | Root; RNG tie issue (PM #11). | OK w/ caveat |
| C2 | Three gtests match. | In scope (plus listed `wave_stage_cell.py`). The match stage is switched with no flag, against rule 4 for v8 gm wave plans. | C1 merged; 15 / 2 / M / slot 6: matches. | Root. | CONFLICT (PM #15) |
| D2 | Closed form plus equality with Python fixture values (the lane can generate them). | Relocates the atx-impl `strategy_ic_{shrink,theme_resid,theme_erc,theme_tsmom,two_speed}` TUs and needs `atx-impl/CMakeLists.txt`; neither is in §2.2's wave-2 D2 row. | D1, B1 merged; 16 / 2 / L / slot 5: matches. | Yes for the fixtures; root does X-5 fit identity. | CONFLICT (minor scope; PM #17) |
| E2 | No named tests, so the acceptance contract cannot be reviewed. | K-P9-9 v2 goes in `wave_manifest.py` (v1 at :60). Run-dir naming "imported by the wave" is `wave_context.py:120-141`. The helpers to consolidate sit in stage_chain.py:72, run_bounded_research.py:40, wave_manifest.py:86, wave_steps.py:113-115, wave_context.py:29, wave_stage_util.py:58-68, cycle_verdict.py:48-58 and backtest_integrity.py:563. None of these is in E2's row. Dropping the `--help` probe and the flag rewrite (:676-687, :950-952) changes v8 argv (the automatic `--no-composition`), against its own "plan diff empty". | E1, D1, C1, A1 merged; 17 / 2 / L / slot 1: matches. | "Kinds fixtures pass on tiny-world" is impossible in wave 2 for walk-forward (D3) and the new cost model (C3). How G-P2's 10 kinds map to K-P9-9's 8 is unstated. | CONFLICT (PM #14) |
| S2 | Gtests match. `OpSig.PredicatesAgree` risks being a tautology. No test for HAC-rule selection or the AuditExact token. | The row lists alpha headers, `factory/{ic_screen,op_catalog}.cpp` and `ic_screen_config.hpp`. The deliverables also need `src/alpha/{typecheck,oracle,registry}.cpp`, `factory/ic_screen.hpp`, `strategy_ic_runner.cpp` (flags; refusals at :638-640 and :874), `strategy_ic_signal_cache.cpp` (AuditExact token), `strategy_ic_result_cache.cpp:114-124` (`min_coverage` key) and `strategy_ic_admission.cpp:237-239` (delisting-returns refusal). The label column's data source is unstated. | S1 merged; 18 / 2 / L / slot 3: matches. | Root. | CONFLICT (PM #16) |
| AL-COMB | Four gtests match. | No Files-in-scope line. Touches the rule files D2 relocates and the fitter plugin. The zero-trial diagnostic "spec" names no exe or kind. | D1 merged; cell waits for the Y-2 read; 19 / 2 / M / slot 7: matches. | Root does X-5 w identity. | CONFLICT (PM #17) |
| AL-SIG | No gtest names ("planted-leak probe with teeth", "fixture tests"). | No Files-in-scope line. Needs 13F and `indexes/` readers that K-P9-2 does not cover, so effort L is understated. | A2 merged (A3 soft); 20 / 2 / L / slot 8: matches. | Root runs K1 `--plan-only`. | CONFLICT (PM #18) |
| C3 | Gtests match. | In scope (NAV files, `atx-impl/src/book/**`, `atx-impl/CMakeLists.txt`). The new `atx-impl/src/book/` dir shares a name with engine `book/` (AL-CLOCK cites `book/two_speed.hpp`); minor. | C2 merged; 15 / 3 / L / slot 1: matches. | Root. | OK |
| D3 | `ThemeTsmom.ApplyTimeSleevesEqualRecorded (on TRAIN fixtures)` needs real TRAIN outputs that a blind lane cannot open. | No Files-in-scope line; needs runner TUs and E2's cycle / manifest files. K-P9-8 id collision. | D2, E2 merged; 16 / 3 / L / slot 2: matches. | The TRAIN fixture is not lane-checkable. | CONFLICT (PM #19; blind-rule risk) |
| AL-CLOCK | Three gtests match. | No Files-in-scope line. `ea_days_since` exists (`research_fields_sec.py`, `registry.json`). | C3 merged + DEC-16; the brief omits plan §3.2's "Y-5 read" precondition; 19 / 3b / M / slot 3: matches. | Root. | OK w/ caveat |
| AL-DATA | No tests named. | No Files-in-scope line; gated. | A3 merged + OD-P9-4..6; 20 / 3 / M / slot 4: matches. | Root. | OK w/ caveat (conditional conflicts 1c #3, #5) |
| PRE | Doc lane; no Root-verifies line (the PM rules it). | Sprint dir only. | §2.1 needs (AL-COMB, AL-SIG, B2, E2) vs §3.2 (adds the AL-CLOCK report) vs the DAG (ALK -> PRE); 13 / 3 / M. | Yes (SHA recomputation, arithmetic). | CONFLICT (ordering; PM #1) |
| A4 | No gtest names (probes and fixture identity). | No Files-in-scope line. | A3 merged; 12 / 3 / L / slot 5, stretch: matches. | Root. | OK w/ caveat (duplicates AL-SIG's 13F reader; PM #18) |

Pools, waves, efforts and merge slots in the briefs all match plan §2.1, §3.2 and §3.3 (22/22). No pool is reused
within a wave, and pools 1, 2, 6 and 10 are never used (DEC-19 holds).

---

## 3. Wave-1 existence spot-check at `d7c1c520`

| lane | path or cite in the brief / plan | status | note |
|---|---|---|---|
| E1 | `scripts/run_bounded_research.py:92-94` | OK | `0 < seconds <= 600` |
| E1 | `run_bounded_research.py:120` | OK | `output.mkdir(..., exist_ok=False)` |
| E1 | `scripts/wave_manifest.py:225-229` | OK | `marginal.seconds` must be a positive int, no upper bound |
| E1 | `scripts/research_cycle.py:369-376` | OK | `validate_runner_phases` |
| E1 | `scripts/wave_steps.py:181-183` | OK | |
| E1 | `scripts/tests/test_wave_speed.py:80-86` | OK | 720 at :80 and :86 |
| E1 | `scripts/wave_stage_preflight.py:72` | OK | single `admission_cycle_prefix` |
| E1 | `research_cycle.py:1176` | OK | done iff `summary.json` exists |
| E1 | `scripts/wave_readers.py:79-81` | OK | returns None when the CSV is missing |
| E1 | `scripts/wave_stage_util.py:58-68` | OK | `phase_rows`, no exe SHA |
| E1 | `research_cycle.py:1015-1019` | OK | ref skipped when fields match |
| E1 | `backtest_integrity.py:1094-1118` | OK | real path `atx-impl/tools/backtest_integrity.py` (unqualified in plan and brief; there is no `scripts/` copy) |
| E1 | `scripts/tests/test_research_spec.py:60-91` | OK | NULL_PINS file list |
| E1 | `scripts/specs/v8/waves/y-s.json:13 / :27 / :41` | OK | `v8ys` / `v8x` / 720. `y-s.head.json:17` also says 720 (R0-2 names both). No other spec exceeds 600. |
| E1 | `research_wave.py`, `cycle_resume.py`, `cycle_verdict.py`, `wave_queue.py`, `atx-engine/tools/stage_chain.py` | OK | all exist |
| A1 | prep:194-278, 411-440, 480-504 | OK | `FIELDS`, `ISSUER_FIELDS`, `SV_FIELDS` |
| A1 | prep:2916-2931 (`load_prior`), 3186-3187 (holdings register) | OK | |
| A1 | prep:948, 1689, 1754 (sealed-rows key) | OK | |
| A1 | `prepare_research_fields_engine.py:146-160` | OK | `engine_path` |
| A1 | `engine.py:13` -> `test_prepare_research_fields_engine.py` | MISSING | the dead reference A1 is asked to fix; the real test is `tests/fixtures/research_fields/test_research_fields_engine_path.py` |
| A1 | `prepare_research_fields_ohlc.py:11-12` | OK | |
| A1 | `research_fields_holdings.py:1216-1245` | OK | `--reuse` interface |
| A1 | `atx-engine/tools/conftest.py:15` | OK | `bind(superseded())` |
| A1 | `code_fingerprint.py` | OK | |
| A1 | `field_registry.{py,json}` | absent (new) | expected |
| A1 | `atx-engine/tests/fixtures/research_fields/` | OK | generator, two pytests, `expected/` (plus a stray untracked `.mypy_cache/`) |
| A2 | `include/.../research/fields/`, `src/research/fields/` | OK | 12 headers, 14 TUs; `registry` / `manifest` / `reuse` / `producer` new |
| A2 | `research_fields_cli.cpp:28`, `:124-133`, `:116` | OK | `kEngineFields`; name dispatch at 123-133; sealed key |
| A2 | `atx-engine/CMakeLists.txt:266-282` | MISMATCH (partial) | the fields block is 248-282, with the library source list at 252-265, outside the cited range |
| A2 | `atx-engine/tests/research_fields/**` | MISSING | real: `atx-engine/tests/research/research_fields_{clock,writer,volume_mean,finra_asof,fixture,cli}_test.cpp` |
| A2 | `fixture_test.cpp:141-200` | OK | real path `tests/research/research_fields_fixture_test.cpp` (200 lines); :197 asserts the old key |
| T1 / G-P4 | `atx-engine/tests/CMakeLists.txt:364-367` | OK | comment block; the target is at 368-381 and has no CTest registration |
| B1 | fit:1130-1170 (`factor_record`) | PARTIAL | `factor_record` starts at :1160; 1130-1159 are `Context` methods |
| B1 | fit:1572-1583, 1613-1671, 1641-1670 | OK | `newey_west_t`, `screen_v4`, greedy pass |
| B1 | `strategy_exposures_verb.{hpp,cpp}` | OK | 69 / 268 lines |
| B1 | `eval/hac.hpp:171` | OK | `mean_inference(x, Kernel, lag, small_sample_correction)` |
| B1 | `atx-impl/tools/equity_strategy_targets.cpp` | OK | 28 lines of verb `if`s; `atx-impl/CMakeLists.txt:205` shows the `target_sources` append pattern |
| B1 | `scripts/wave_rules.py:50-51` | OK | sign 0 kept |
| C1 | `strategy_nav_v7.cpp:103` | OK | `thread_local active_state` |
| C1 | nav:2299-2304, 3220-3221 | OK | `--book-workers` and grid refused under the v7 hook |
| C1 | v7:503, 535, 1121-1133, 349-430, 1107, 1134 | OK | |
| C1 | `replay_cost.cpp:92` | OK | `std::pow(participation, delta)` |
| C1 | `strategy_cost_v2.cpp:45` | MISMATCH | `std::cbrt`, not `pow` |
| C1 | `strategy_cost_v2.cpp:138, 196, 231` | OK | `pow`; 138 and 196 raise to `s2.impact_delta` |
| C1 | nav:800; `strategy_live_test.cpp:2234` | OK | `AdvHold.CapacityCurveCarriesTheCapInBothPasses` |
| C1 | `strategy_vol_target.*`, `strategy_risk_target.*` | OK | |
| D1 | admission.cpp:500-526, 814-835, 233-287, 246-285, 283-285 | OK | 4-row table at 513-518 |
| D1 | runner.cpp:282-289; detail.hpp:146-176 | OK | 20-parameter `score_role`; `PinnedWeights` |
| D1 | `strategy_ic_theme_resid.hpp:24-29`; `strategy_two_speed.hpp:27-32` | OK | 13 themes each |
| D1 | fit:247-267, 280-291, 2337-2473 | OK | |
| D1 | `atx-impl/strategies/alphas/registry.json` `themes` | OK | dict of the 13 themes |
| D1 | `atx-impl/tools/composition_rules.py` | EXISTS | 390-line ew-theme-std-v1 mirror inside D2's 1,535-line deletion set |
| S1 | marginal.cpp:52-92, 359-407 | OK | copied helpers; `return_guard` |
| S1 | `marginal_rank_ic.hpp:45` | OK | `kMaxMarginalRegressors = 11U` (book + 10 themes) |
| S1 | `orthogonalize.{hpp,cpp}` | OK | |
| S1 | `signal_cache.cpp:41-151, 93-95`; `result_cache.cpp:29-48, 45-48, 114-124` | OK | no build-flavour token |
| S1 | `CMakePresets.json` "Release equity preset" | ALREADY EXISTS | `equity-rel` at :48-55; `rel-avx2` at :149-156 |
| S1 | engine `research_return_guard` | OK | `atx-engine/{include,src}/.../data/role_panel.*` |
| T1 | `scripts/tests/fixtures/tiny_world.py`, `test_cycle_e2e.py` | OK | 392 / 267 lines; e2e skips without `ATX_EQUITY_BIN` |
| T1 | `scripts/research-build.ps1` | OK | 142 lines; `ValidateSet('equity-dev','equity-rel')`; no `-Canary` yet |
| T1 | `nav_summ.py:420-463`, `backtest_integrity.py:160-486`, `dsr_total.py` | OK | |
| T1 | `eval/{deflated_sharpe,min_trl,pbo,trial_clusters,perf_metrics}.hpp` | OK | no paired / Memmel / CBB anywhere in `eval/` |
| T1 | `generate_from_spec.py --spec specs/library-v71.json` | OK | under `atx-impl/strategies/` (paths relative to it) |
| T1 | class-C list (audit §4) | OK | 10 `generate_*` files found by glob in `atx-impl/strategies/` (the audit counts 11, including a check script) |

---

## 4. DS review delta (§7 and §3-§6 notes vs the S1 / S2 briefs)

| # | DS item | what S1 / S2 (or another lane) says | delta |
|---|---|---|---|
| 1 | §7.1 Release IC exe and build token | S1: token for build type, NDEBUG, CRT and xsimd; Release target; oracle and conformance suites under Release. | Omitted: DS §3 "CRT ops log / exp / pow / tanh (vm.hpp:236-258) not covered by the one-library identity check". S1 has no CRT-op probe, so X-5's u / w identity is the only Release check, and a library using `tanh` / `pow` may differ (the same class as NAV's `pow`). The preset already exists. "Point exes.ic at it" is a root spec step; fine. |
| 2 | §3 `ic_identity` takes its FP flavour from the impl TU | S1 covers it. | Scope gap: needs an accessor in `ic_screen.cpp` (S2's file in wave 2, unowned in wave 1). |
| 3 | §7.2 parallel serial kernels | S2: band-split by `chunk_axis`, per-worker scratch like `ts_col_thr_`, columns extracted once in `eval_lit_ts`, parity at 1 / 4 / 16. | Omitted: recurrences (DS §5 serial list), and re-measuring E-24's worker cap of 4. The speed-up needs more than 4 workers, and no lane or ruling owns that. |
| 4 | §7.3 survival-conditioned labels | S2: `--label-terminal imputed-v1`; report both rules. | Omitted: where the label column's data comes from, and the admission refusal of delisting-returns roles for IC (`strategy_ic_admission.cpp:237-239`) plus the `role_panel.cpp:41-52` guard. Neither is in S2's scope. |
| 5 | §7.4 coverage gate as config | S2: `min_coverage`, default .8, keyed in the IC-result scope. | Omitted: "report a coverage-restricted IC with its own n". Identity note: adding the key to the scope JSON (result_cache.cpp:114-124) changes every cache key even at .8, so all cached X-5 IC results miss once (outputs unchanged, one cold pass). Key it only when it differs from the default, or accept the cold pass. |
| 6 | §7.5 one `OpSig` table | S2: family / axis / needs_group / stateful, a `static_assert`, derived predicates, derived `lookahead_safe`, sweeps that include `formulaic_ops()`. | Contradiction risk: deriving streaming `is_cs_op` from the table adds CsBucket / CsResidOn, which streaming omits today (DS §2). That is a behaviour change on the streaming path, not flag-absent identity, and the brief does not flag it. Also omitted: "exhaustive switches for kernel dispatch only". |
| 7 | Also: memory model and printing `required_bytes` over the cap | D1 covers it. | OK. |
| 8 | Also: make AuditExact the default | S2 allows it with the cache under its own token; switching the default is a P9-B0 ruling. | A deliberate deferral, consistent with the plan. OK. |
| 9 | Also: unify the HAC rules (`ic_screen.cpp:203` vs `hac.hpp:309-313`) | S2: named and selectable, default unchanged, both printed. | Softened from "unify" (the plan's choice): by default the t-stats stay non-comparable. The fixed-b note (lag 126 on ~750 days at h=63) is omitted. |
| 10 | §2 Group dtype inferred from the `grp_` prefix | K-P9-1 declares `dtype` (A1). | Omitted: no lane makes the typecheck (`typecheck.hpp:151-157`, S2's in wave 2) or the runner's manifest binding use the declared dtype, so the silent-F64 hazard stays. |
| 11 | §3 look-ahead gaps (hand-set `lookahead_safe`, per-opcode truncation, YOPS tested only streaming == batch, slot sweep missing `formulaic_ops`) | S2 covers all four. | OK. |
| 12 | §5 the w pass reloads and SHA-verifies every cached signal (signal_cache.cpp:385-387) | S1's "no re-hash" covers the marginal verb only. | Omitted (minor). |
| 13 | Previous finding O3 (no sharing across candidates; SubtreeCache off under a mask) | The plan lists O3 as an S2 input. | The brief delivers nothing for it (minor; fine if intended). |
| 14 | §4 two IC engines, average ranks x6, Pearson x6 | No lane (D1 handles centred rank x3). | Omitted; outside P9. |
| 15 | §4 flags bypassing the spec (`--help` probing, raw `ic.flags`) | E2. | Covered, but blocked by the K-P9-6 shape (PM #7). |
| 16 | §6 planted-IC end-to-end test; per-opcode sweep | T1; S2. | OK. |
| 17 | §3 memory: theme planes one at a time; date-blocked VM (O6) | No lane. | Omitted (O6 stands); acceptable. |
| 18 | DS-1 "NAV stays Debug until its reordered sum is found" vs NV-3 | The plan has C1's probe decide (§2.4). | OK, but C1's `sqrt` scope (PM #10) decides whether the probe can pass. |

---

## 5. Plan mandates a reviewer would treat as defects, and test-first implications

| # | item | mandated by | why a reviewer flags it | suggested fix |
|---|---|---|---|---|
| 1 | Guard tests whose allowlist equals the current state: T1's `test_no_python_mirror.py` and `test_no_versioned_scripts.py`, A1's `test_no_new_python_builder.py` | G-P5, G-P6, DEC-5; T1 and A1 briefs | They pass at merge whatever the lane wrote, and the "expiry column" / "lane that retires each" has no mechanical check. | Every row names an existing path (a stale row fails) and an expiry the test can evaluate (e.g. once the replacing C++ file or target named in the row exists, the row must be gone). |
| 2 | `OpSig.PredicatesAgree (streaming and VM)` | S2 brief | Once every predicate derives from one table, agreement holds by construction, so the test asserts nothing. | Pin the derived predicate sets against today's hand-written lists as golden sets, with the CsBucket / CsResidOn streaming change as the one ruled difference. |
| 3 | T1 tie-gtest rows for paired CBB dSR, Memmel SE and house DSR | T1 brief (3); K-P9-5 "T1 pins the fixture first"; DEC-6 "tie test lands first" | The engine code arrives only with B2, so this is a test written to fail first. That is test-first, against the owner's no-TDD directive, and root's "tie gtest green" cannot hold. | T1 commits the values; B2's `EvalVerb` test asserts them (PM #11). |
| 4 | B1 comparator pytest "checks the C++ outputs' committed fixture bytes" | B1 brief (3) | A lane that cannot build has no C++ bytes; a hand-made fixture shaped to the C++ pins the implementation. | The Python fitter generates the fixture, and `FactorsVerb.EqualsFixture` compares the C++ against it. |
| 5 | Reuse and seal rule implemented in Python (A1) and in C++ (A2) in the same wave | plan §2.4 A1 (4)-(5), A2 | The plan itself creates a Python copy of a C++ rule, which its own G-P5 forbids. | An allowlist row with expiry at the A3 / A4 slice, or A1's entry hands reuse of engine rows to the exe. |
| 6 | D1's Python rule plugin list beside the C++ `CompositionRule` table | DEC-8 | A dispatch-only mirror of the rule registry until D2. | Acceptable if listed in the G-P5 allowlist with D2 as the expiry. |
| 7 | `ThemeTsmom.ApplyTimeSleevesEqualRecorded (on TRAIN fixtures)` | D3 brief | Needs recorded TRAIN sleeves, i.e. real 2020-2023 outputs a blind lane cannot open. | A synthetic fixture in the gtest; TRAIN equality becomes root's identity run. |
| 8 | Lanes that name no tests: E2 (none), AL-SIG / AL-DATA / A4 (no gtest names), A1 (no Tests line), E1 (7 deliverables untested) | the briefs | Rule 7 says the named tests are the acceptance contract, so a reviewer cannot check these. | Name them before dispatch. |
| 9 | B1 "admission.csv reproduced byte for byte" | plan §2.4 B1; B1 brief | Float t-stats from a numpy BLAS dot vs a C++ sequential sum differ by ULPs, so the criterion is unachievable or forces a hand-tuned match. | Decisions and order byte-equal; floats within 1e-12 with the reason (as K-P9-4 allows) (PM #12). |
| 10 | RNG-driven statistics tied at tolerance 0 / 1e-12 (CBB p-values, ONC k-means, seeded PBO) | T1, B2, G-P7 | Needs a bit-exact port of numpy's PCG64 and `Generator.integers`, which no text mentions. | Commit the draw indices in the fixture and tie each statistic given those draws (PM #11). |
| 11 | Test-first check | briefs rule 7: "Implement first, then the tests named in the brief" | No brief asks for test-first inside a lane. The only test-before-code ordering is cross-lane (T1's fixture before B2, DEC-6). | OK, except row 3. |
| 12 | The A1 / A2 rename leaves role manifests on the old key (`prepare_recent_research.py:863`) | A1 scope | Two names for one count across manifest families. | State it in the reports, or rename both (the role builder is out of scope). |
| 13 | C1 `ReplayCostSqrt.*` "probe test of cost_fraction bits" | C1 brief | A probe that only checks `sqrt == pow(.5)` inside one build says nothing about Debug vs Release. | Pin expected bits computed in Python (correctly rounded sqrt); root runs the probe in both builds. |

---

## Conflicts needing a PM ruling

Each item gives the conflict, the text that mandates each side, and a proposed ruling as decision -- why -- cost if
wrong.

1. **Dispatch inputs, base SHA and PRE ordering.**
   - Side A: every brief's Read line and rule 1 cite plan §0.6 / §2.2-§2.3, `docs/plans/2026-10-02-p9-code-review*.md`,
     the literature review and status 7. The base is `d7c1c520` (plan §2.1, §3.3).
   - Side B: those 11 files are untracked in pool-2 (`??`), so a pool leased at `d7c1c520` does not have them. Plan
     §1's intro says "frozen base (d7c1c520 + P0-FIX)", but P0-FIX does not exist at R0-1. PRE is in wave 3 (§2.1,
     §3.3), yet §3.2 makes the AL-CLOCK (3b) report a hard need.
   - Ruling: root commits the 11 docs as one doc-only commit on the v8 branch before R0-1 and names it the wave-1
     frozen base (P0-FIX not in it; E1 alone owns those files). PRE goes out after AL-CLOCK's report, or as a two-pass
     lane. -- Lanes must read the contracts they code against, and docs move no code byte. -- Cost if wrong: one
     extra commit in v8 history; no effect on identity.

2. **Who edits the test CMake files.**
   - Side A: plan §2.2 gives `atx-impl/tests/CMakeLists.txt` and `atx-engine/tests/CMakeLists.txt` to T1; A2's brief
     forbids "CTest lines (T1)".
   - Side B: C1, D1, B1 and A2 must add gtests to targets with explicit source lists (`-strategy-target-tests`
     :114-134, `-strategy-ic-tests` :95-112, `atx-engine-research-fields-tests` :368-374). B1's admission tests need
     a new target. G-P4 wants every research target registered.
   - Ruling: lanes add cases to already-listed test TUs where natural. Where a new TU or target is unavoidable, they
     append one line or block as a listed cross-lane edit. T1 edits only registration / label lines and also
     registers the names fixed now (e.g. `atx-engine-research-admission-tests`). -- Otherwise root's named targets
     silently skip the new tests. -- Cost if wrong: textual merge conflicts that root resolves (cheap).

3. **E1's access to `research_cycle.py`.**
   - Side A: E1's scope allows "task 0 only: research_cycle.py (the lines named)" (plan §2.2 agrees).
   - Side B: E1's wave-1 deliverables live in that file: OR-2 `lock` / exes pins (:543-552, `lock()` :1793), OR-3
     resume binding (:883-911, 1071-1077, 1323-1332), OR-4 auto-advance (:906-910, 1646-1647), and K-P9-10
     `build_type` from BUILDS (:223-227).
   - Ruling: E1 owns `research_cycle.py` for the whole of wave 1 (S1's entry moves to E1 per #4); E2 splits it in
     wave 2 on top. -- No other wave-1 lane needs the file. -- Cost if wrong: E1 ships half of OR-2..OR-4, or a
     reviewer BLOCKs it for scope.

4. **`--candidates` plumbing.**
   - Side A: S1's brief calls it a "one-line cross-lane edit" to `MARGINAL_SPEC_FLAGS`. Plan §2.4 says "E1 owns the
     wave call site". E2's brief adds it "if S1 did not".
   - Side B: `MARGINAL_SPEC_FLAGS` accepts digit values only (:211, :528-532), and E1's brief has no call-site task.
   - Ruling: S1 ships the exe flag only. E1 adds `--candidates` as a step-built option (in `MARGINAL_BUILT`, ids taken
     from the wave's candidate list) behind a manifest `speed` key; absent means today's argv. E2 drops its fallback.
     -- The wave knows the candidate ids; a spec cannot. -- Cost if wrong: Y-S-sized waves keep the K² marginal
     (CM-1).

5. **A1 / A2 file and fixture ownership.**
   - Side A: plan §2.2 gives A1 `prepare_research_fields*.py`, and A1 deliverable (6) edits `engine.py:13`; A1's
     fixture scope is "generator only". A2's scope includes `engine.py`, promises "existing fixture identity
     unchanged" and renames the key "with A1". Plan §2.4 A2 turns the engine shim into a `kind: engine` row in A1's
     JSON.
   - Side B: one file has two owners. The rename moves `expected/manifest.normalized.json`,
     `test_research_fields_fixture.py` and `research_fields_fixture_test.cpp:197`, which neither scope covers. A2's
     test path `atx-engine/tests/research_fields/**` does not exist.
   - Ruling:
     - A2 owns `engine.py`; the :13 fix moves to A2.
     - A1 owns `field_registry.json`, writes every row as `kind: python`, regenerates `expected/` and updates the
       fixture pytest for the rename.
     - A2 gets one listed edit that flips the three engine rows (`kind: engine`, `builder` = BuilderKind id), and it
       updates the key literal in the C++ fixture test.
     - A2's test path is corrected to `tests/research/research_fields_*`.

     -- Merge order A1 (3) then A2 (4) makes this sequential. -- Cost if wrong: the fixture test is red at slot 4.

6. **K-P9-3 `producer` shape.**
   - Side A: K-P9-3 defines `producer: {kind, exe_sha256, git_sha, build_type, receipt_sha256}` and names A1 as the
     reader.
   - Side B: today's entries hold `producer: {module, code_sha256, code_sha256_lf, code_git_blob_sha1}`, and reuse
     reads it (prep:2795, 2853). A1's brief is silent.
   - Ruling: `producer.kind` discriminates. "python" keeps today's four keys plus `kind`; "engine" carries K-P9-3's
     keys. A1's reuse reader branches on `kind`, and A1's entry writes the manifest of a mixed build. Add one line to
     A1's brief. -- This keeps v15 reuse working. -- Cost if wrong: engine entries are recomputed and the manifest
     churns on every build.

7. **K-P9-6 / K-P9-7 shape.**
   - Side A: K-P9-6 is a bare array `[{id, version, block_key, stage, params_schema, incompatible[],
     recipe_sha256}]`. D1's struct is `{id, block_key, parse, verify, apply_stage, recipe_text, working_bytes}`.
   - Side B: E2 must replace the `--help` probe for "no-composition" / "marginal" (:676-687) with list-rules, and
     D1's struct cannot emit `version`, `params_schema` or `incompatible`.
   - Ruling: both list-rules verbs emit `{"schema", "capabilities": [...], "rules": [...]}`, and D1's struct gains
     `version`, `params_schema` and `incompatible`. Fix this before D1 and C1 merge. -- One envelope serves E2 for
     both exes. -- Cost if wrong: E2 keeps the `--help` probe (small), or the contract breaks in wave 2.

8. **Centred-rank helper.**
   - Side A: D1's brief asks for "one centred-tied-rank helper (3 C++ copies)".
   - Side B: the copies are in `composition.cpp` (D1), `marginal_rank_ic.cpp:98-129` (S1-owned) and the unowned
     engine `group_rerank.hpp`.
   - Ruling: D1 moves `composition.cpp` onto engine `group_rerank.hpp` (a listed engine edit) and leaves
     `marginal_rank_ic.cpp` to S1 or wave 2. -- Keeps files disjoint per wave. -- Cost if wrong: two copies remain
     for one more wave.

9. **The PM7-35 sign rule's Python side.**
   - Side A: B1 writes both predicates and "the PM picks one before merge" (plan §2.4, B1 brief).
   - Side B: the Python twin, `wave_rules.py:50-51`, belongs to E1 in wave 1, and no brief edits it.
   - Ruling: rule now to keep today's wave rule (sign 0 kept, R-2 precedent), so wave 1 needs no Python change. The
     sign rule in `wave_rules.py` then becomes a G-P5 allowlist row expiring at D2. -- Gate and wave stay consistent
     with no scope change. -- Cost if wrong: one E1 edit later if the stricter gate rule is wanted.

10. **C1's `sqrt` scope.**
    - Side A: C1's brief keeps `pow` "for other exponents (strategy_cost_v2.cpp:45,138,196,231)".
    - Side B: :138 and :196 raise to `s2.impact_delta` = .5 (NV §3), :45 is `cbrt`, and C1's own root check demands
      a bit-identical Release NAV.
    - Ruling: apply `delta == .5 -> sqrt` at `cost_v2.cpp:138` and :196 as well (same probe); keep `cbrt` and :231
      with a note; root's Release compare includes a capacity-curve cell. -- Otherwise G-P3's NAV half fails on
      capacity / v7 cells. -- Cost if wrong: one more re-pin of cost bits (already planned under DEC-11).

11. **T1's tie fixture vs B2.**
    - Side A: T1's gtest must reproduce paired CBB dSR, Memmel SE and house DSR "from the engine headers", root
      expects "tie gtest green", and K-P9-5 says the fixture comes first.
    - Side B: those three exist only in Python (nav_summ.py:329-372, backtest_integrity.py:1254), and CBB / ONC / PBO
      use numpy Generator draws.
    - Ruling: T1 commits every Python value plus the draws (CBB starts, ONC / PBO randoms). Its wave-1 gtest asserts
      only what the engine already implements. B2's `EvalVerb` test asserts the rest from the same fixture using the
      committed draws. -- No test written to fail first, no PCG64 port. -- Cost if wrong: B2 cannot tie at 0 / 1e-12,
      and the Python deletion (G-P5 / G-P7) stalls.

12. **B1 admission bytes.**
    - Side A: plan §2.4 B1 and the brief's root check require "admission.csv reproduced byte for byte".
    - Side B: float t-stats come from numpy `e @ e` (BLAS order) vs C++ sequential sums, and K-P9-4's 1e-12 escape
      covers only the factor series.
    - Ruling: decisions, status, first-failure reason and admitted order are byte-equal; float columns tie within
      1e-12 with the reason stated. -- Same logic as K-P9-4. -- Cost if wrong: B1 is returned at root identity for
      ULP noise.

13. **G-P9 seal refusal.**
    - Side A: A1's readers refuse a mismatched seal (brief (5)); G-P9 says "every field consumer".
    - Side B: the fields manifest in `tiny_world.py` has no seal block (T1's canary, E1's tests), and the C++
      consumers (IC exe, NAV) are assigned to no lane.
    - Ruling: refuse only a seal that is present and different (absent = legacy, with a warning). Give the C++ half
      to S2 (IC exe, wave 2) and C3 (NAV, wave 3), or amend G-P9. -- Keeps tiny-world working. -- Cost if wrong:
      G-P9 is unticked at the freeze.

14. **E2's scope and acceptance.**
    - Side A: plan §2.2's E2 row lists `research_cycle.py`, `research_spec.py`, `research_add_alpha.py` and
      `cycle_admission.py`. The brief's acceptance says every v8 spec and manifest plans byte-identical argv and "the
      kinds fixtures pass on tiny-world".
    - Side B:
      - K-P9-9 lives in `wave_manifest.py` (:60).
      - The helpers to consolidate sit in `wave_context.py`, `wave_steps.py`, `wave_stage_util.py`,
        `cycle_verdict.py`, `stage_chain.py`, `run_bounded_research.py` and `backtest_integrity.py` (B2's).
      - Dropping the flag rewrite (:950-952) changes v8 argv.
      - The walk-forward and cost-model kinds need D3 / C3.
    - Ruling:
      - In wave 2, E2 inherits E1's wave-1 files, except `wave_stage_cell.py` (C2) and `wave_stage_record.py` (B2).
      - The sha256 copy in `backtest_integrity.py` stays for B2.
      - Capabilities change source, but the automatic v8 argv flag stays.
      - Ship 8 runnable kind fixtures plus 2 schema-only ones (walk-forward, cost model) checked by `wave plan`.
      - G-P2 stays a phase-3 gate (plan §4.3).

      -- This matches what E2 must physically edit. -- Cost if wrong: E2 is blocked, or BLOCKed for scope.

15. **C2's match stage.**
    - Side A: C2's brief says "the match stage calls it" (`wave_stage_cell.py`).
    - Side B: rule 4 requires a flag-absent build to be byte-identical, and E2's acceptance covers v8 manifests.
    - Ruling: a manifest key (e.g. `gross_match: "in-process"`) selects in-process calibration; absent means the
      two-process match. `wave_rules.py:110-119` is deleted in a later slice, after root's identity run. -- Keeps v8
      plans identical. -- Cost if wrong: the E2 / §4.2 identity check fails on gm manifests.

16. **S2's scope.**
    - Side A: plan §2.2's S2 row lists alpha headers, `ic_screen.cpp`, `op_catalog.cpp` and `ic_screen_config.hpp`.
    - Side B: the brief's work also needs `src/alpha/{typecheck,oracle,registry}.cpp`, `factory/ic_screen.hpp`, the
      runner (flags; :638-640, :874), the signal cache (AuditExact token), the result cache (:114-124) and admission
      (:237-239), plus a data source for the label-only column.
    - Ruling: add those files to S2's wave-2 scope (no other wave-2 lane owns them). The label column is the role
      builder's delisting-returns / imputed close, read as a label-only field, with the refusal relaxed for that
      column only; root builds the role. -- Cost if wrong: S2 ships flags with no data behind them, or BLOCKs on
      scope.

17. **D2 vs AL-COMB.**
    - Side A: D2 relocates the `strategy_ic_*` rules into `atx-engine-research-composition` (the atx-impl files
      become thin includes) and owns the fitter.
    - Side B: AL-COMB adds `strategy_ic_theme_hedge.*` in atx-impl, a row in D1's table and a schedule rule beside
      theme-tsmom, and it needs a fitter plugin entry for its weights block.
    - Ruling: AL-COMB rebases on D2 (slot 5, then 7) and writes its rule in D2's layout, with listed cross-lane edits
      for one table row, one plugin entry and the schedule hook. D2's row is amended to include the atx-impl rule TUs
      and an append to `atx-impl/CMakeLists.txt`. -- Cost if wrong: rebase churn, or a rule the fitter cannot reach.

18. **AL-SIG source readers vs A4.**
    - Side A: AL-SIG must ship `gia_13f` and `russell_recon` as C++ builder kinds on K-P9-2 in wave 2.
    - Side B: K-P9-2 covers TickerHistory3 only. The C++ 13F reader is A4's (wave 3, stretch), and no reader or
      builder exists for the `indexes/` Russell proxy (lit F8: "missing").
    - Ruling: AL-SIG builds both readers under `research/fields/sources/` on A3's interface (a minimal 13F rows
      reader with the filing-lag clock, and an `indexes/` reader); A4 reuses AL-SIG's 13F reader. -- A4 is a stretch
      lane and P9-S needs these signals. -- Cost if wrong: two 13F readers, or P9-S with one family.

19. **K-P9-8 schema id.**
    - Side A: DEC-9 / K-P9-8 name `atx.composition-weights/v2` and say "v1 = one row, byte-identical to today's
      file".
    - Side B: today's files use `atx.dsl-composition-weights/v1` and `/v2`, where v2 means the theme block
      (`strategy_ic_admission.cpp:22-25`, marginal:198, `composition_rules.py:64-65`, fit:298-301).
    - Ruling: time-indexed weights become `atx.dsl-composition-weights/v3`, and v1 / v2 stay untouched. -- Avoids two
      "v2" schemas in the runner. -- Cost if wrong: the runner parses the wrong weights format.
