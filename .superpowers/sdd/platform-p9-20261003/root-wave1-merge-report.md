# Root wave-1 merge report (P9)

Root = `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Recipe: `root-wave1-merge-brief.md`, plan §3.4 / §4.1.
Split by the PM ruling (progress.md, R0-14 block): M1a = slots 1-4 (this section), M1b = slots 5-7, M1c = slot 8,
M1d = the post-merge gates. Later agents append their own sections below.

## M1a: slots 1-4 (E1, T1, A1, A2)

Agent root-m1a, 2026-10-03. Integration head at dispatch: `6a68d7f9` (confirmed). Python is
`"C:/Program Files/Python312/python.exe"`, every pytest with `PYTHONDONTWRITEBYTECODE=1 -q -p no:cacheprovider`, run
from the root tree. Logs of every pytest run are kept in the session scratchpad (`pyt-<slot>-<suite>.log`).

### Two pre-existing failures at the integration base (not from any merge)

**(1) pytest.** `scripts/tests/test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` fails at `:119`
(`assert set(EXCLUDED_BY_CLASS) <= set(v9) - read`; extra items `noa_lag4`, `ea_window_pre5`, `ins_cluster_buy`).
- It fails at `6a68d7f9` itself: the test run alone on a `git archive 6a68d7f9` copy of `scripts`, `atx-impl/strategies`,
  `atx-impl/tools`, `atx-engine/tools` gives `1 failed`, the same three names.
- Cause: v8 commit `a5914373` ("wave y-s: register library v8ys (15 frozen strings ...)") added alphas to
  `atx-impl/strategies/alphas/registry.json` that read those three fields, which the test's `EXCLUDED_BY_CLASS`
  constant still lists as unread. `a5914373` is in the P9 base but in no lane base (not an ancestor of `d7c1c520` or
  `3fa2dd4a`), so every lane ran green and root's first full run is the first to see it.
- No merge in this group touches the test, `scripts/specs/v8/base-lo1.json` or `registry.json`
  (`git diff --stat 6a68d7f9 HEAD --` on the three paths is empty).
- Not a slip and not this group's to fix: the test pins prereg item 4 (the mined-fields rule); editing its constant
  or the registry is a PM ruling. Every `scripts/tests` count below includes this 1 failure under both seeds.

**(2) gtest.** `ResearchFieldsWriter.QuantilesPartitionLikeNumpy` (`research_fields_writer_test.cpp:139`, x5:
`same_bits(x, 0.0)` false for the quantiles of `{-0.0}`) and `ResearchFieldsVolumeMean.SumOrderIsNumpys`
(`research_fields_volume_mean_test.cpp:120/133/135`: got 5e15, 1.25e15, 1.25e15 for 1/3, 0, 1/9) fail in
`atx-engine-research-fields-tests --gtest_filter=ResearchFields*`. Attributed to unchanged v8 code, not to A2:
- the two test TUs and the code under test (`trailing_mean.cpp`, `field_writer.cpp`, `field_stats.cpp` and their
  headers, `research_fields_test_support.hpp`) have no diff `6a68d7f9..HEAD`; their last commits (`eccf6338`,
  `e812a0fd`, `ffbbcb56`) are ancestors of the lane base `d7c1c520`;
- neither test object was recompiled by p9-1a / p9-1b (`research_fields_volume_mean_test.cpp.obj` and
  `research_fields_writer_test.cpp.obj` dated 2026-10-02 21:47, `trailing_mean.cpp.obj` 21:45);
- the VolumeMean values follow from `trailing_mean.cpp:57` (`accepted = present && isfinite(v) && v >= 0.0`: the
  test's -1e16 is rejected as a negative volume, so (1e16 + 1) / 2 = 5e15 and (1e16 + 2) / 8 = 1.25e15), i.e. the
  test expects negatives accepted and the code (since `eccf6338`) rejects them.
- `atx-engine-research-fields-tests` was not in CTest before T1 (T1: "unregistered before"), which is why nobody saw
  it; T1 now registers it under `atx_research`, so `ctest -L atx_research` will show these 2 failures.
- A2's named expectations (7 Registry, 8 Manifest, 3 Fixture, 3 Cli green) all hold (slot 4).

**Both are open for the PM: M1d's gates ("scripts/tests 0 failed", "ctest -L atx_research") cannot pass until they are
ruled.** I did not fix either (neither is a merge slip; both touch pinned test expectations).

### Slot 1: E1 tasks 1+ (`fdd2bda3`, pool-17)

- **E1-STALE re-confirmed read-only before the merge:** `research_cycle.py wave status scripts/specs/v8/waves/<w>.json
  --root C:/atx-wt/pool-2` for y-s, y-3, y-2, y-5: each exit 0, 9/9 stages `done`. Re-run after the merge: the same
  output byte for byte (E1's code reads the state the same way).
- **Merge:** `git merge --no-ff fdd2bda3` -> **`28cd7d8c`**. No conflict. 26 files, Python only (scripts/*,
  `atx-impl/tools/backtest_integrity.py`, `atx-engine/tools/stage_chain.py` + test, the E1 report).
- **Build:** none (no C++ / CMake in the lane).
- **pytest** (all after the merge, sequential):

  | command | result | exit | wall |
  |---|---|---|---|
  | `scripts/tests/run_two_seeds.py` (scripts/tests, PYTHONHASHSEED 0 and 1) | seed 0: 1 failed, 352 passed, 4 skipped; seed 1: 1 failed, 352 passed, 4 skipped (the failure is pre-existing (1) above, both seeds) | 1 | 875 s |
  | `-m pytest atx-engine/tools` | 349 passed, 6 subtests passed | 0 | 168 s |
  | `-m pytest atx-impl/tools` | 625 passed, 2 skipped, 17 subtests passed | 0 | 187 s |

- **Identity (E1 merge note 5):** `research_cycle.py wave plan scripts/specs/v8/waves/y-s.json` before and after the
  merge:

  | output | expected | observed | match |
  |---|---|---|---|
  | `wave plan y-s.json` stdout+stderr | same lines as before the merge | pre: exit 0, 10 lines, sha256 `990cad41446ca904`; post: exit 0, 10 lines, sha256 `990cad41446ca904`; `cmp` identical | yes |
  | `wave status` y-s / y-3 / y-2 / y-5 | unchanged, 9/9 done | identical to pre-merge, 9/9 done, exit 0 each | yes |

  (The lane's tiny-world argv/file identity is the lane's own scratch evidence; no new manifest sets a `driver` key.)
- **Slips fixed:** none. Merge note 2 (MARGINAL_CANDIDATES into MARGINAL_BUILT) is for after S1 (slot 5, M1b).

### Slot 2: T1 (`0a61b706`, pool-19)

- **Precondition** (brief "T1"; on the root tree after slot 1, before the merge): `cd atx-impl/strategies && $PY
  generate_from_spec.py --spec specs/library-v71.json --check` -> exit 0:
  `fund_industry_ic_v71.json 787c802e... 39317 bytes`, `fund_industry_ic_v71.recipe.v2.json 69e95298... 25389 bytes`,
  `frozen: 19 artefacts verified; plan (alphas/fixtures/v71_plan_k1.json): 48 K1 rows within budget`. So the whole
  head merged (class-C deletion `dee23d2a` + allowlist `10493041`); the dd5c4f5e-only fallback was not needed. The
  brief's form (committed fixture plan) was run; the T1 report's `--plan-json <live --plan-only>` variant needs an IC
  exe run on data and was not asked by the brief.
- **Merge:** `git merge --no-ff 0a61b706` -> **`769ca3d3`**. No conflict. 38 files (CMake test lists, eval_tie
  fixture, 11 class-C generators + 9 tests deleted, canary, guards, `research-build.ps1 -Canary`).
- **Build:** none under its own tag. T1 has no C++ source; its two CMake edits (deferred `atx_research`
  registration in `atx-engine/tests` and `atx-impl/tests`) were configured by A2's build p9-1a (slot 4) and checked
  there by the CTest count (75 + A2's 15 = 90, see slot 4). Canary goldens are not recorded here (after C1, later agent).
- **Post-merge checks:** `generate_from_spec.py ... --check` again: exit 0, the same three lines (787c802e, 69e95298,
  19 artefacts, 48 K1 rows). `generate_eval_tie.py --check`: `eval_tie: tied`, exit 0. `git grep` for the 11 deleted
  module names outside `.superpowers`/`docs`/JSON: only docstrings, the v61 dry-run fixture text and the guard's
  frozen rows; no import.
- **pytest:**

  | command | result | exit | wall |
  |---|---|---|---|
  | `scripts/tests/run_two_seeds.py` | seed 0: 1 failed, 373 passed, 4 skipped; seed 1: 1 failed, 373 passed, 4 skipped (pre-existing (1) only); prints `G-P5: 37 mirrored rule function(s) in 14 row(s) left for P9 lanes` | 1 | 791 s |
  | `-m pytest atx-impl/strategies` | 14 passed | 0 | 3 s |
  | `-m pytest atx-impl/tools` (tiny_world fixture users) | 625 passed, 2 skipped, 17 subtests passed | 0 | 200 s |
  | `-m pytest atx-engine/tests/fixtures/eval_tie/test_eval_tie_fixture.py` | 7 passed | 0 | 1 s |

- **Identity:** the slice-2 `--check` above (expected v71 `787c802e`, recipe.v2 `69e95298`, 19 artefacts, exit 0 --
  observed equal, before and after the deletion). The live canary skipped (no `ATX_EQUITY_BIN`), as intended until
  C1.
- **Slips fixed:** none.

### Slot 3: A1 (`f8edca96`, pool-12)

- **Merge:** `git merge --no-ff f8edca96` -> **`de925dca`**. No conflict. 26 files, Python + JSON + the fixture's
  `expected/manifest.normalized.json`.
- **Build:** none (A1 changes no C++; its `expected/` key rename makes `ResearchFields*` gtests red until A2, per
  its report).
- **Registry acceptance** (from `atx-engine/tools`, A1 "How root verifies" 1): `import prepare_research_fields as b,
  field_registry as fr; ... fr.check(...)` -> `ok 92`, exit 0. `field_registry.json` sha256 `6c56b739...3e51`,
  200,282 bytes (LF, `.gitattributes -text` held on checkout).
- **pytest** (tools and fixtures in separate processes, A1 minor m5):

  | command | result | exit | wall |
  |---|---|---|---|
  | `-m pytest atx-engine/tools` | 399 passed, 6 subtests passed | 0 | 178 s |
  | `-m pytest atx-engine/tests/fixtures/research_fields` | 9 passed, 1 skipped | 0 | 6 s |
  | from `atx-engine/tools`: `-m pytest test_field_registry.py test_no_new_python_builder.py test_field_module_imports.py test_prepare_research_fields_reuse_keys.py test_prepare_research_fields_seal.py` | 50 passed | 0 | 13 s |

- **Identity:** the v15 `--registry` run is after A2 + the flip (slot 4), as the brief orders.
- **Slips fixed:** none.

### Slot 4: A2 (`2b6f8e6f`, pool-13), the 3-row flip, build, identities

- **Merge:** `git merge --no-ff 2b6f8e6f` -> **`b50c0a1f`**. One textual conflict, `atx-engine/tests/CMakeLists.txt`:
  T1 and A2 both appended at the file end (T1's deferred `atx_research` registration, A2's `target_sources` of
  `research_fields_registry_test.cpp` / `research_fields_manifest_test.cpp`, ruling P2). Resolved by keeping both,
  T1's block first (its comment says wave-1 lanes append below it; the call is deferred, so it still registers the
  target A2 extends). No other conflict.
- **3-row flip** (A2 listed edit, P5): **`a97d0483`** `data(fields): flip si_shares, si_dtc, vol_126 to kind engine`.
  Done through A1's round-trip, not by hand (scratch `flip_registry.py`, fresh interpreter from `atx-engine/tools`,
  repository window): load + `bind` + `check` the committed file; assert `dump(regenerate(file)) == file`; set the
  three rows `kind: engine`, `builder: <row name>` in memory; write `dump(regenerate(ns, doc))`.
  - Changed: exactly 6 values (kind and builder of the 3 rows); 92 rows, same order, 0 engine-only rows.
  - sha256 `6c56b739...3e51` (200,282 B) -> **`835b93ead136e79fc5c22cfcad1184cc2068ca912a5790dbe98ef4a9a8168480`**
    (200,237 B). No test or script pins the registry SHA (`git grep 6c56b739` outside docs: none).
  - `check` after the flip: `ok 92`, and `dump(regenerate(load())) == file bytes` (round-trip holds).
- **Build p9-1a** (FAILED, slip): `research-build.ps1 -Tag p9-1a -Targets
  "atx-engine-research-fields,atx-engine-research-fields-tests,atx-research-fields"` (equity-dev, 4 jobs, free 4,538
  MiB at admission; my gate 3,072 = 1,536 build estimate + 1,536; min free sampled 3,959) -> exit 1 after 30.0 s,
  12 TUs, 0 links: `research_fields_cli.cpp(243,7): error: variable 'i' is incremented both in the loop header and in
  the loop body [-Werror,-Wfor-loop-analysis]`.
- **Slip fixed:** **`96c0cfee`** `fix(fields): option loop steps by two, no increment in the body (integration of P9
  A2)`: `parse_args` advances `i += 2` in the header and drops the body's `++i` (each option takes one value; same
  parse, same refusals). A2 wrote the C++ without compiling it, by rule (its report's concern 1).
- **Build p9-1b** (source `96c0cfee`, DirtyEntries 2 = `progress.md` + the png): same 3 targets, 3 jobs (free 3,073 at
  admission; gate 3,072 met at 3,081; min free sampled 2,318) -> **exit 0, 19.2 s wall, 9 TUs, 3 links, 0 warnings**
  (UTF-16 build log decoded: 0 lines with "warning", 0 with "error"; p9-1a: 0 warning lines, its 2 error lines are the
  slip). Executables: `atx-engine-research-fields-tests` `1900fc4a...94a3`, `atx-research-fields`
  **`666ae58fde7bd0de14e6c32003b35679a067e065dd3471113aa2ad68bbecd473`**. ConfiguredProvenance
  `a97d0483...-dirty`: the generated build identity is configure-time (p9-1a's configure, before `96c0cfee`), so the
  exe's K-P9-3 `git_sha` reads `a97d0483...-dirty` -- stale by design (A2 open risk 2; reuse keys on `exe_sha256`).
- **gtests** (`build-equity\bin\atx-engine-research-fields-tests.exe`, vcpkg debug + release bin on PATH):

  | filter | result | exit |
  |---|---|---|
  | `ResearchFieldsRegistry.*` | 7 passed | 0 |
  | `ResearchFieldsManifest.*` | 8 passed | 0 |
  | `ResearchFieldsFixture.*:ResearchFieldsCli.*` | 6 passed (3 Fixture + 3 Cli; the two A1-dependent ones green) | 0 |
  | `ResearchFields*` | 44 passed, 2 failed: `ResearchFieldsWriter.QuantilesPartitionLikeNumpy`, `ResearchFieldsVolumeMean.SumOrderIsNumpys` = pre-existing (2) above | 1 |

- **CTest count** (`atx-build.ps1 -Ctest -Preset equity-dev -N -L atx_research`, listing only): **Total Tests 90** =
  46 `atx-engine-research-fields-tests` (T1's 31 + A2's 7 + 8) + 44 `atx-impl-strategy-mine-tests`, as T1 predicts
  (75 at the T1 head + A2's cases). `-N -L atx_equity_strategy`: 537 (no pre-merge count was taken; no lane in this
  group touches that label).
- **pytest** (after the flip and the build):

  | command | result | exit | wall |
  |---|---|---|---|
  | `ATX_RESEARCH_FIELDS_EXE=<root>\build-equity\bin\atx-research-fields.exe -m pytest -rs atx-engine/tests/fixtures/research_fields/test_research_fields_engine_path.py` | 11 passed, 0 skipped (lane: 9 passed, 2 skipped): `test_real_executable_identity` and `test_repository_registry_routes_the_ported_rows_to_the_engine` now run and pass | 0 | 8 s |
  | `-m pytest atx-engine/tools` (separate process) | 399 passed, 6 subtests passed (A1's registry tests incl. `test_lane_a2_flip_keeps_the_registry_tests_green` hold with the flip) | 0 | 171 s |
  | `-m pytest atx-engine/tests/fixtures/research_fields` (separate process, no exe env) | 12 passed, 1 skipped (the real-exe test) | 0 | 6 s |

- **Identity 1: root's v15 `--registry` run** (A1 report "How root verifies" 3). Argv built by script from v15's
  recorded build-B receipt (`train-2020-2023-lo3-fields-v15-run/receipt.json`) with exactly A1's substitutions:
  one-liner dropped (script invoked), `--registry atx-engine/tools/field_registry.json` prepended, `--fields` = v15's
  84 names in v15 manifest order, `--reuse build-equity/train-2020-2023-lo3-fields-v15 --reuse-sha256 26fee5ce...`,
  `--output build-equity/p9-a1-identity-fields-v15`; every other option unchanged (incl. `--reuse-hardlink`,
  `--max-rss-mib 2048 --max-seconds 580`); no `--engine-exe`. Through `run_bounded_research.py --output
  build-equity/p9-a1-identity-fields-v15-run --seconds 600 --max-rss-mib 1536 --min-free-mib 512` (+ 24 `--bind`s:
  v15's set with v15's manifest for v15a's, plus `field_registry.py/.json`, `prepare_research_fields_engine.py`,
  `code_fingerprint.py`). Cap 1,536: measured peaks of this builder on this role are 838 MiB (v15) and 709 MiB (v15a);
  gate 3,072 (first launcher at cap 2,048 / gate 3,584 waited, was stopped before it launched anything, no run dir).
  Gate met after 15 s at 3,181 free; **outcome completed, exit 0, 38.9 s, peak tree RSS 315 MiB, min free 2,968 MiB**;
  receipt sha256 `88deb651865a298c75e4efb08398ff1f47193471a6e3aaa3ec80ab531635a02d`; new manifest
  `7e830c111832afa58eb35d25813c32dd4cd3f91fb2516e804a6c77750c582762`. stderr: the expected notice "engine rows
  si_shares, si_dtc, vol_126 are computed by the Python builder (no --engine-exe)" (all three were reused).
  Compared by SHA and JSON path only (scratch `m1a_v15_compare.py`, `m1a_esr.py`):

  | file / JSON path | expected (A1 report) | observed | match |
  |---|---|---|---|
  | 84 payloads: `fields[i].sha256`, `files[<name>.f64]` sha256 + bytes, re-hash of every new `.f64` | byte-identical to v15 | 84/84 equal; 0 re-hash mismatches; same 84 names, same order | **yes** |
  | `reuse.reused` / `reuse.computed` | 82 / [`nt_first_126`, `deal_pending`], reason "inputs differ" | 82 / [`nt_first_126`, `deal_pending`]; `not_reused` both "inputs differ (stage manifest or bridge SHA-256)" | yes |
  | `reuse.prior_runtime_versions` | null (v15 is a legacy prior) | null | yes |
  | `seal.exclusive_end` | 2024-01-01 = v15's | 2024-01-01 | yes |
  | top-level `code_sha256`, `code_sha256_lf`, `code_git_blob_sha1` | may differ | 3 paths differ (`code_sha256_lf` 74df97f9 -> 71ed02ab) | yes |
  | `runtime_versions` | new | added (duckdb 1.5.1, numpy 1.26.4, pyarrow 18.0.0, python 3.12.2) | yes |
  | `reuse` block | may differ | 14 paths (from, manifest_sha256, lists, `runtime_rule`, `prior_runtime_versions`, ...) | yes |
  | 82 reused entries' `reused_from` | dir = v15 | 244 paths; every reused entry's `reused_from.dir` is v15 | yes |
  | `nt_first_126`, `deal_pending`: producer identity, `imported_code` | may differ | `producer` 6 paths, `imported_code` 3 paths; `reused_from` removed on both (2 paths: computed now, v15 had them from v15a) | yes |
  | `source_checks` rename `rows_available_on_or_after_2025_dropped` -> `rows_sealed_dropped` at 10 paths | 10 paths | 8 in `source_checks` (deals/identity_bridge, issuer/{fund_events, identity_bridge, sic_events}, sec/identity_bridge, si_dtc, si_shares, v9/nt_first_126/identity_bridge) + the 2 `v9/earn_season_rank/*` ones inside the moved block (next row); every other `source_checks` group equal modulo the rename | yes |
  | `source_checks.v9.earn_season_rank` | not in A1's list (A1 review minor: "report's expected root diff misses source_checks.v9 earn_season_rank move", progress.md:90) | **moved** to `reuse.prior_source_checks_of_partial_groups.v9.earn_season_rank` (v9 is now a partial group: earn_season_rank reused, nt_first_126 computed); content equal to v15's block modulo the 2-key rename | **outside the report's list** (predicted by the review only) |
  | `source_checks.holdings.code.{code_git_blob_sha1, code_sha256, code_sha256_lf}` | not in A1's root list (its evidence calls `code_sha256*` "documented keys") | **changed**: `code_sha256_lf` 9c025522 -> 2c524455 = the file hash of `research_fields_holdings.py`, which A1 rewrote (`register` -> `bind`); the rest of `source_checks.holdings` is equal; all 9 holdings payloads reused, byte-identical | **outside the report's list** |

  **Result: payload identity holds (84/84, 82 reused + the 2 predicted recomputes), but the manifest differs at two
  JSON-path groups outside A1's "How root verifies" list.** Both are metadata, neither moves a payload byte, and
  one was flagged by A1's review; under the dispatch rule ("any difference outside what a report predicted: stop")
  I stop here for a PM ruling (proposed: both are the expected consequence of A1's code edit and of the partial v9
  group; add them to the A1 identity list / DEC-20 substitution list).

- **Identity 2: A2's TRAIN identity of the three engine fields** (A2 report). v2 spec
  `build-equity/p9-a2-train-identity-spec/spec.json` (role `build-equity/train-2020-2023-lo3` pin `e1c67101...`,
  `output_dir` `build-equity/p9-a2-train-identity-fields`, fields `si_shares, si_dtc, vol_126`, finra
  `C:/atx/data/finra_short_interest`), run as `run_bounded_research.py --output ... --seconds 600 --max-rss-mib 1536
  --min-free-mib 512 --build-type Debug -- build-equity\bin\atx-research-fields.exe build --registry
  atx-engine\tools\field_registry.json --spec <spec> --receipt <spec dir>\a2-train.receipt.json`.
  - Attempt 1 (`p9-a2-train-identity-run`): outcome process-error, exit 1 in 0.3 s, "spec: output_dir is not a
    directory" -- my slip (the exe wants an existing empty `output_dir`; the A2 report says "new empty dir"); nothing
    was written. Attempt 2 (`p9-a2-train-identity-run2`, the empty dir created): **completed, exit 0, 27.5 s, peak 192
    MiB**, free 4,525 before / 4,036 min; "built 3 field(s), reused 0"; runner receipt `fd454e3b...8c76`; manifest
    `7ac5f8fb...7338`.

  | field | expected (v15 manifest `sha256`) | observed (`fields[i].sha256` = `files[<name>.f64]` = disk re-hash) | match |
  |---|---|---|---|
  | si_shares | `780362070cd40b46...` | `780362070cd40b46...` | yes |
  | si_dtc | `f424d90fd46ba573...` | `f424d90fd46ba573...` | yes |
  | vol_126 | `ee15d4bf1e2a564c...` | `ee15d4bf1e2a564c...` | yes |
  | each entry's `producer` | K-P9-3 `{kind: engine, exe_sha256, git_sha, build_type, receipt_sha256}`, `exe_sha256` = the exe = research-build's `Executables` | `kind engine`, `exe_sha256 666ae58f...` = exe on disk = runner `executable_sha256` = p9-1b receipt; `build_type Debug`; `git_sha a97d0483...` (stale, see build) | yes |

- **Trial ledger:** `build-equity/trials.jsonl` 133 lines, sha256 prefix `27e40f9f` (unchanged). 0 trials.

### M1a commits (first-parent, on `6a68d7f9`)

| commit | what |
|---|---|
| `28cd7d8c` | merge E1 tasks 1+ (`fdd2bda3`) |
| `769ca3d3` | merge T1 (`0a61b706`) |
| `de925dca` | merge A1 (`f8edca96`) |
| `b50c0a1f` | merge A2 (`2b6f8e6f`), CMakeLists conflict: both blocks kept |
| `a97d0483` | registry flip (3 rows to kind engine, via `regenerate()`) |
| `96c0cfee` | slip fix: `-Wfor-loop-analysis` in `research_fields_cli.cpp` |
| (next) | this report, `integration-log.md`, `progress.md` (PM's pending lines, unedited) |

### For M1b (slots 5-7: S1, B1, D1) and later

- Status: M1a **STOPPED at slot 4** on the v15 manifest metadata rows above (payloads identical). Whether M1b may
  start before that ruling is the PM's call; nothing in slots 5-7 reads `field_registry.json` or the fields manifest.
- Next build tag is **p9-1c** (p9-1a failed on the slip, p9-1b passed). Builds reconfigure only on a CMake change;
  the fields exe's `git_sha` stays `a97d0483...-dirty` until then.
- E1 merge note 2: after S1 merges, add `MARGINAL_CANDIDATES` (and S1's other new verb options) to
  `research_cycle.MARGINAL_BUILT` for `test_marginal_argv_is_the_verbs_full_cli`.
- `atx-impl/CMakeLists.txt` / `atx-impl/tests/CMakeLists.txt` tails: T1 appended a deferred `atx_research`
  registration in `atx-impl/tests`; S1 / D1 / B1 append after it -- keep every block (same resolution as slot 4).
- B1's admission target `atx-engine-research-admission-tests` is already named in T1's engine-side deferred list; it
  joins `atx_research` once B1's block defines it.
- The two pre-existing failures (scripts/tests mine rule; the 2 fields gtests) will show in every full run until
  ruled; count them, do not fix them in a merge.
- Host: VS Code held ~3 GB; free memory sat at 2.8-3.2 GB between my runs. A 1,536-cap run's 3,072 gate is usually
  reachable within a minute; a 2,048-cap gate (3,584) was not reached in the several minutes I waited.

## M1b: slots 5-7 (S1, B1, D1)

Agent root-m1b2 (resume of root-m1b, killed mid slot 5), 2026-10-03. Integration head at dispatch: `20443022`
(confirmed: `68ee0c33` design docs, `6476b927` S1 merge, `20443022` MARGINAL_BUILT fix; tree clean but `progress.md`
(PM lines) and the owner's png). Same Python and pytest flags as M1a; pytest only with explicit suite paths
(PY-HYG); scratch logs `gt-*.log` / `pyt-*.log` in the session scratchpad. Slot 5 was re-derived from git and the
`build-equity` run dirs (receipts, exe SHAs, outputs); nothing the killed agent left was taken on its word.

### Slot 5: S1 (`34ef92dd`, pool-18)

- **Merge** (killed agent, verified from git): `git merge --no-ff 34ef92dd` -> **`6476b927`** (16 files: the
  `build_flavor.hpp` token, `marginal_rank_ic`, `strategy_marginal_ic`, new `strategy_marginal_pair_cache`, IC
  identity / source pin `2d758bff` (S1-PIN), `CMakePresets.json` equity-rel deps dir + `equity-rel-ic`, one
  `atx-impl/CMakeLists.txt` tail line, tests). Merged with no conflict (parents `68ee0c33`, `34ef92dd`).
- **E1 x S1 merge note** (killed agent, verified): **`20443022`** `fix(cycle): S1's marginal value options join
  MARGINAL_BUILT` -- `research_cycle.MARGINAL_BUILT` gains `--candidates`, `--pair-cache`, `--verified-digests`; the
  test's verb-option set gains the same three. Python only.
- **Build p9-1c** (killed agent's run, verified by receipt `build-equity/mega-p9-1c-receipt.json`): source
  `20443022` (C++ = the S1 merge; `20443022` is Python only), equity-dev, targets exactly S1's
  `atx-equity-strategy-ic, atx-impl-strategy-ic-tests`, 4 jobs, free 3,442 MiB at admission -> **exit 0, 97.9 s,
  33 TUs, 4 links, 0 warnings** (UTF-16 build log decoded: 0 "warning" lines, 0 "error" lines). Executables
  `atx-equity-strategy-ic` **`4e321143053e9fadd689983917aaab67b4ffa0c1d0ef3ebedd82c8d3dc079b0f`**,
  `atx-impl-strategy-ic-tests` `efff612a...18a3`; both re-hashed on disk now = the receipt (no later build touched
  them). The engine-side copy of `combine_marginal_rank_ic_test.cpp` is compiled into `atx-impl-strategy-ic-tests`
  (no separate engine target is configured under `ATX_EQUITY_ONLY`).
- **gtests, Debug** (run by me; S1 report's anchored filter
  `MarginalIc.*:IcIdentity.*:CombineMarginalRankIc.*:StrategyIcRunner.IcSourcesPinnedToSemanticsVersion:StrategyIcRunner.VmSourcesPinnedToSemanticsVersion`):
  **31 passed / 0 failed**, exit 0, 14.8 s (MarginalIc 15 incl. `CacheRootsNeverShareAcrossBuilds`, IcIdentity 1,
  CombineMarginalRankIc 13, StrategyIcRunner 2 pins).
- **gtests, Release: deferred to M1d.** pool-2 has no Release tree (`build-equity-rel` absent; S1 moved equity-rel's
  deps to a fresh `deps/equity-rel`), so the first Release configure fetches deps and compiles the IC closure from
  scratch -- not cheap under the ~3 GB host budget. M1d's Release IC adoption (G-P3) builds that tree anyway; run
  `MarginalIc.*` (and S1's full filter + the whole binary) there.
- **pytest** (touched suite: `20443022` edits `scripts/research_cycle.py` + `scripts/tests/test_research_cycle.py`;
  the S1 merge touches no Python):

  | command | result | exit | wall |
  |---|---|---|---|
  | `PYTHONHASHSEED=0 -m pytest scripts/tests/test_research_cycle.py` | 92 passed, 3 skipped | 0 | 50 s |
  | `PYTHONHASHSEED=1 -m pytest scripts/tests/test_research_cycle.py` | 92 passed, 3 skipped | 0 | 48 s |

- **Identity: flag-absent Debug X-5 marginal** (S1 report step 3 / fix round 1 step 3). The killed agent's two runs
  were reused after proving their provenance; I re-ran nothing.
  - argv = the v8 X-5 lineage's marginal step (X-5 = `x-theme-erc` -> `lib-v8x3b-gm`, T1 review): exactly the argv
    of v8's `mega-v8-b0b-train-u-v8x3b-marginal-poolonly-run` (candidate cache `mega-candidate-cache-v8-lo3`, library
    `fund_industry_ic_v8x3b.json` `32f8d69f`, pool `848612980`, role lo3 `e1c67101`, themes `c67edffa`, fields v13,
    `--min-names 1000`), only `--output` changed; through `run_bounded_research.py` (360 s, cap 1,024 MiB), both
    under source `20443022`, code pathspec clean.
  - **old exe** = `build-equity/p9-m1b-ref-ic/atx-equity-strategy-ic.exe`, sha256 **`985019d9...c989`**. Proof of
    origin: it is the `Executables.atx-equity-strategy-ic` of research-build receipt `mega-v8-16d-receipt.json`
    (tag v8-16d, Source `e0fd0297`, equity-dev, 0 dirty), file mtime 2026-10-02 21:51:41 (after `e0fd0297`'s
    21:47 commit), and the exe of the P9 R0 runs (`p9-r05-*`, `p9-r09-p13-w-run`). `e0fd0297` is an ancestor of the
    lane base `d7c1c520` and `e0fd0297..68ee0c33` changes no source of this exe (C++ diff: `strategy_ic_runner_test.cpp`
    (tests only) and the `atx-engine-research-fields` library, which `atx-impl-core` does not link) -- so it is the
    d7c1c520 / pre-S1 Debug IC exe in code.
  - **new exe** = `build-equity/bin/atx-equity-strategy-ic.exe` `4e321143...` = p9-1c.

  | run dir | exe | outcome | wall | peak RSS | `marginal_ic.json` sha256 |
  |---|---|---|---|---|---|
  | `p9-s1-x5-marginal-old(-run)` | old `985019d9` | completed, exit 0 | 198.8 s | 253 MiB | `6e4f084c...53ad` |
  | `p9-s1-x5-marginal-new(-run)` | new `4e321143` | completed, exit 0 | 178.2 s | 252 MiB | `1556aaee...527d` |

  | compared | expected (S1 report) | observed | match |
  |---|---|---|---|
  | bytes outside the `stage_seconds` object | byte-identical | 41,991 B each with the object replaced by a marker, sha256 `778a9472...de4a` both; `cmp` equal | **yes** |
  | `stage_seconds` | gains labels, hash, read, kernel, pairwise | JSON paths differ only at `/stage_seconds/{hash,kernel,labels,pairwise,read}` (added) and `/stage_seconds/{stream,total}` (timings) | yes |
  | `build_vm_identity` | legacy identity unchanged, legacy cache roots read | `dslvm1_clang18.1` (no flavour suffix) in both; 58 candidates read from `mega-candidate-cache-v8-lo3` (warm, no recompute) | yes |
  | old vs v8's recorded output (`mega-v8-b0b-train-u-v8x3b-marginal-poolonly`, exe `67f72921`) | -- (extra check) | equal outside `stage_seconds` | yes |

  **Result: flag-absent identity holds.**
- **Speed (S1 report step 5, logged only):** same argv with `--workers 8 --pair-cache build-equity/p9-s1-x5-pair-cache`
  (cold cache; killed agent's run `p9-s1-x5-marginal-w8-run`, exe `4e321143`, verified the same way): completed,
  **129.3 s wall** (vs 178.2 s at 1 worker, 198.8 s old), peak 297 MiB; stdout `computed_pairs=1653 cached_pairs=0`,
  kernel 889.9 CPU-s summed over 8 workers. Its `candidates` block equals the 1-worker run; it differs only at
  `/inputs/pair_cache`, `/workers`, `/working_bytes` (362,136,670 -> 407,280,558) and `stage_seconds`. No warm
  pair-cache rerun was made (the pairwise stage is 7.5-11.7 s of the wall; the kernel dominates).
- **Slips fixed:** none. The `20443022` merge note was the planned E1 x S1 follow-up, not a slip.
- **Trial ledger:** 0 trials.

### Slot 6: B1 (`01f20405`, pool-14)

- **Merge:** `git merge --no-ff 01f20405` -> **`1ab97798`**. Three list-tail conflicts (ruling P2), each resolved by
  keeping both blocks, the integration branch's first: `atx-engine/tests/CMakeLists.txt` (T1's deferred
  `atx_research` registration + A2's `target_sources`, then B1's `atx-engine-research-admission-tests` target),
  `atx-impl/CMakeLists.txt` (S1's pair-cache `target_sources`, then B1's `strategy_factors_verb.cpp` + its private
  Debug `/O2`), `atx-impl/tests/CMakeLists.txt` (T1's deferred registration, then B1's `target_sources` of
  `strategy_factors_verb_test.cpp`). Only the marker lines were removed, plus one blank separator line in each tests
  file; the diff against HEAD is exactly B1's three blocks. `atx-engine/CMakeLists.txt` (B1's library + exe block)
  auto-merged. T1's deferred engine-side call lists `atx-engine-research-admission-tests`, which this block defines.
- **Pre-build reference:** before the build overwrote it, `build-equity/bin/atx-equity-strategy-targets.exe`
  (`72ff6d2d...5f25`) was copied with the bin DLLs to `build-equity/p9-m1b-ref-targets/` (mtime kept). It is the
  v8-16d receipt's `atx-equity-strategy-targets` (Source `e0fd0297`), i.e. pre-S1 and pre-B1 code.
- **Build p9-1d** (source `1ab97798`, DirtyEntries 2 = the owner's png + `docs/plans/2026-10-03-p9-sprint-plan.md`,
  another agent's uncommitted edit outside the code pathspec; equity-dev, 4 jobs, free 4,842 MiB at admission):
  targets exactly B1's five `atx-equity-strategy-targets, atx-impl-strategy-target-tests,
  atx-engine-research-admission, atx-research-admission, atx-engine-research-admission-tests` -> **exit 0, 31.3 s,
  12 TUs, 6 links, 0 warnings** (UTF-16 log: 0 "warning" lines; its one "error"-matching line is PowerShell 5.1
  wrapping cmake's stderr notice `-- GLOB mismatch!`, the re-glob that reconfigured for B1's new test files, not a
  compiler message). Executables: `atx-equity-strategy-targets`
  **`5149aab9ecce386f5ff7506fd7d2416750aa203c46521d41ef7aec6db5f3e33b`**, `atx-impl-strategy-target-tests`
  `033aa33c...67f4`, `atx-research-admission` **`416966bc38c2d8d7d6715340edd2919aa27102fce4cc9447caba2b60e838d32e`**,
  `atx-engine-research-admission-tests` `c2b35ce7...1461`. B1 wrote the C++ without compiling it; it compiled first
  time (no slip).
- **gtests** (Debug):

  | binary / filter | result | exit |
  |---|---|---|
  | `atx-impl-strategy-target-tests --gtest_filter=FactorsVerb.*` | 4 passed (EqualsFixture, FutureReturnDoesNotChangePast, VerbWritesTheKP94Layout, RefusesBeforeAnyOutput) | 0 |
  | `atx-engine-research-admission-tests --gtest_filter=ResearchAdmission*` | **16 passed** (the binary lists 16; the report's "17" is the review's m9 miscount) | 0 |
  | `Exposures.*` | not in any B1 target: the suite is compiled only into `atx-impl-tests` (glob), which B1 does not name. Deferred to M1d's impl test exe run; the exposures verb is covered below by the pytest export test on the new exe and the X-5 NAV identity | - |

- **pytest** (B1 "How root verifies" 3 and its neighbours; vcpkg bins on PATH):

  | command | result | exit | wall |
  |---|---|---|---|
  | `ATX_EQUITY_TARGETS_EXE=<bin>\atx-equity-strategy-targets.exe ATX_RESEARCH_ADMISSION_EXE=<bin>\atx-research-admission.exe -m pytest -rs atx-impl/tools/test_factor_series_admission.py` | **8 passed, 0 skipped** (lane: 7p/1s; the end-to-end test of both exes now runs) | 0 | 2 s |
  | `-m pytest atx-impl/tools/test_exposures_export.py atx-impl/tools/test_horizon_stats.py` (no exe env) | 11 passed, 1 skipped (needs the exe) | 0 | 5 s |
  | same, with `ATX_EQUITY_TARGETS_EXE` = the new exe | 12 passed, 0 skipped (`test_verb_export_reproduces_the_fitters_factors` on the p9-1d exe) | 0 | 5 s |

- **Identity 1: existing verb bytes unchanged** (B1 step 4: "X-5's nav ... must stay byte-identical"). X-5's NAV
  argv exactly as v8 recorded it (`mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1414-v8x3b-run`: `nav
  --combined mega-v8-r1w-train-std-v8x3b-1/train_combined.json` `7f9a6bc5`, role lo3, fields v13, label role
  lo3-dlret, `aim-partial-v5`, L 1.1414, `--capacity-curve`, ...), only `--output` changed; bounded runner 300 s, cap
  1,536 MiB, `--admission-wait-seconds 600`, source `1ab97798`.

  | run | exe | outcome | wall | peak |
  |---|---|---|---|---|
  | `p9-b1-x5-nav-old(-run)` | `p9-m1b-ref-targets` `72ff6d2d` (v8-16d, pre-S1/B1) | completed, exit 0 | 77.6 s | 587 MiB |
  | `p9-b1-x5-nav-new(-run)` | `bin` `5149aab9` (p9-1d: S1 + B1 in `atx-impl-core`) | completed, exit 0 | 70.8 s | 586 MiB |

  | compared (SHA-256 per file, whole tree) | expected | observed | match |
  |---|---|---|---|
  | old vs new | all files byte-identical | 27 / 27 files identical (5 daily CSVs, 5 events CSVs, recipe, summary, v7_extras, v7 TC CSV, capacity_curve.csv, `capacity/*`); none on one side only | **yes** |
  | old vs v8's recorded X-5 NAV dir (exe `a95f6f0a`) | (extra check) | 27 / 27 identical | yes |

- **Identity 2: admission comparator on real data** (B1 step 5; P12: decisions and order byte-equal, floats within
  1e-12 x max(1, |x|)). Scratch scripts `b1_signals.py`, `b1_factor_compare.py`, `b1_admission_compare.py`,
  `b1_real.py`. Inputs exactly each fit's own: role lo3 `e1c67101`, the library / orientations / runner-summary SHAs
  of the fit receipt; `entries = [CacheLayout.resolve(c, role) for c in library]`; fitter records = the WorkStore
  (`build-equity/fit-work`) records the X-5 and Y-S fits reused (all present; one context digest each, equal to that
  fit's `admission.json inputs.context_sha256`). Then `factors` (bounded, cap 1,536) and `atx-research-admission
  screen --screen v4-prior-v1 --candidates <fit>/admission.json` (bounded, cap 1,024).
  - Attempt 1 on X-5 (`p9-b1-x5-factors-run`): process-error, exit 1 in 16.8 s, `InvalidArgument: factors: candidate
    value_composite: IC runner: factors signal payload value_composite extent`. **My slip**: I wrote the fitter's
    repo-relative payload paths into `signals.json`; the verb resolves a relative payload against the signals DIR
    (`strategy_factors_verb.cpp:117`, as its report states), so the file was not found (the shared `load_pinned_f64`
    reports a missing file as "extent"). Nothing was written. Fixed in the input (`p9-b1-x5-factors-sig2`: absolute
    payload paths, each file size checked = 1,405 x 5,922 x 8 B), no code change. Note for E2 / D2, who will write
    `signals.json`: B1's recipe ("write `signals_document(entries)`") needs absolute paths or paths relative to DIR.

  | check | X-5 (v8x3b, 58 candidates; fit `mega-weights-v8-r1-std-v8x3b`) | Y-S (v8ys, 73 candidates; fit `mega-weights-v8x-theme-erc-v8ys`) |
  |---|---|---|
  | `factors` run | completed, exit 0, 35.9 s, peak 507 MiB; 1004 decisions x 58, 0 refused | completed, exit 0, 43.8 s, peak 507 MiB; 1004 x 73, 0 refused |
  | verb candidate order = library order | yes | yes |
  | `compare_factor_series` (flat decisions exact; f, tau <= 1e-12) | **0 differences** (worst abs gap f 1.2e-16, tau 8.9e-16) | **0 differences** (f 1.8e-16, tau 1.1e-15) |
  | `screen` run | completed, exit 0, 0.27 s | completed, exit 0, 0.25 s |
  | `compare_admission_csv` (P12) | **0 differences** | **0 differences** |
  | every row's cell count = the header's (review m4: the helper's `zip` would hide a missing cell) | 58 rows x 24 cells, both files | 73 rows x 24 cells, both files |
  | non-float cells (ids, statuses, failed checks, redundancy, ranks, signs, cache entries, order) | byte-equal | byte-equal |
  | float cells | 284 differ in text; worst relative gap 5.4e-15 | 358 differ in text; worst 5.4e-15 |
  | manifest `admitted` order / `counts` / `sign_conflicts` vs the fit's `admission.json` | equal / equal / equal (47 admitted) | equal / equal / equal (61 admitted) |
  | whole-file bytes | differ (float text: numpy pairwise vs sequential sums, as the lane predicts) | differ (same) |

  Output SHAs (prefix): X-5 `factor.f64` `9854cfb9`, `manifest.json` `e09e3bac`, C++ `admission.csv` `3ee7a705` (fit
  `b6cb8a74`); Y-S `manifest.json` `5f7b7c11`, C++ `admission.csv` `6a15a9dc` (fit `3fd17187`).

  **Result: B1 identity holds** (existing NAV bytes unchanged; X-5 and Y-S admission equal under P12).
- **Slips fixed:** none in code (the signals-path slip was in my own input file).
- **B1 merge note** (`load_pinned_f64` from S1's TU, D1's header): built after S1 here; rebuilt after D1 in slot 7.
- **Trial ledger:** 0 trials (reader runs; no ledger line).
