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
  - argv = the marginal step of library v8x3b, owned by X-5's parent X-3 (`lib-v8x3b-gm.json`; X-5 =
    `x-theme-erc-gm.json`, a child spec that inherits the parent's u / marginal outputs and changes only the fit): exactly the argv
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

- **Identity 1: existing verb bytes unchanged** (B1 step 4: "X-5's nav ... must stay byte-identical"). The NAV argv
  of X-5's parent X-3 (`lib-v8x3b-gm`; I first took it for X-5's own -- X-5's own NAV, `x-theme-erc-gm` at L 1.1720,
  is run in slot 7 on the post-D1 exe) exactly as v8 recorded it
  (`mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1414-v8x3b-run`: `nav
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
  | old vs v8's recorded X-3 NAV dir (exe `a95f6f0a`) | (extra check) | 27 / 27 identical | yes |

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

### Slot 7: D1 (`757ba582`, pool-20)

- **Merge:** `git merge --no-ff 757ba582` -> **`1239a5ff`**. Two list-tail conflicts (ruling P2), every block kept,
  the integration branch's first:
  - `atx-impl/CMakeLists.txt`: S1's pair-cache line + B1's factors block, then D1's `strategy_ic_rules.cpp` block. Not
    a plain marker strip: the conflict hunk ended before an `endif()` that B1's and D1's blocks share textually (git
    took the common last line out of the hunk), so removing only the markers would have left B1's `if()` without its
    `endif()` and given D1's two. Resolved as B1 block + `endif()`, then D1 block + `endif()` (6 `if(` / 6 `endif()` in
    the file).
  - `atx-impl/tests/CMakeLists.txt`: T1's deferred registration + B1's `target_sources`, then D1's `target_sources`
    of `strategy_ic_rules_test.cpp` (a blank separator line added).
  - No D1 file is in either S1-pinned source list (`dsl_vm_sources`, `ic_result_sources`: engine files only), so the
    S1 pin `2d758bff` needs no recompute; no expected hash was touched.
  - The merge was committed from the index only: the tree also held other agents' uncommitted edits
    (`progress.md`, `wave2-carry.md`, `docs/plans/2026-10-03-p9-sprint-plan.md`), left as they were.
- **Pre-build reference:** the S1-only IC exe (`4e321143`, p9-1c) was copied with the bin DLLs to
  `build-equity/p9-m1b-ref-ic-s1/` before the build, for attribution had the D1 identity failed (not needed).
- **Build p9-1e** (source `1239a5ff`, DirtyEntries 4 = the owner's png + the three other agents' files above, all
  outside the code pathspec; equity-dev, 4 jobs, free 4,249 MiB at admission): targets exactly D1's four
  `atx-equity-strategy-ic, atx-impl-strategy-ic-tests, atx-impl-strategy-target-tests, atx-impl-tests` -> **exit 0,
  117.8 s, 41 TUs, 6 links, 0 warnings** (the one "error"-matching log line is again PowerShell wrapping cmake's
  `-- GLOB mismatch!` notice). D1's `Role::memory` risk did not fire (no `-Wmissing-field-initializers` at
  `strategy_research_role.cpp:121`); D1 compiled first time (no slip). Executables: `atx-equity-strategy-ic`
  **`52892590e290b4a7a61a08bc4d1718a084cb1d4b36516ee39320f5c2a5e1fd2b`**, `atx-impl-strategy-ic-tests`
  `609f4005...fe33`, `atx-impl-strategy-target-tests` `fce04cd1...5781`, `atx-impl-tests` `f9eb7a2a...9946`.
- **Build p9-1f = B1's five targets rebuilt after D1** (B1 merge note; source `1239a5ff`, free 3,277 MiB): exit 0,
  6.2 s, **0 TUs** (p9-1e had already compiled `atx-impl-core` with D1), 1 link, 0 warnings. `atx-equity-strategy-targets`
  relinked -> **`f0ee3908fd16dec755cde89f36b06b8ee451185b8b6df827a959ad75a032683c`**; `atx-impl-strategy-target-tests`
  = p9-1e's `fce04cd1`; `atx-research-admission` `416966bc` and `atx-engine-research-admission-tests` `c2b35ce7`
  unchanged from p9-1d (engine library, no atx-impl dependency).
- **gtests** (Debug, all exit 0, 0 failed):

  | binary / filter | result |
  |---|---|
  | D1 new: `atx-impl-strategy-ic-tests --gtest_filter=CompositionRules.*:IcAdmission.*:ThemeRegistryRunner.*` | 8 passed (CompositionRules 6 incl. ListRulesJsonPinned / StageListIsTheV8CallSequence, IcAdmission 1, ThemeRegistryRunner 1) |
  | D1 regressions: `atx-impl-strategy-ic-tests --gtest_filter=StrategyIcRunner.*:CompositionV8.*:ThemeResidRunner.*:ThemeTsmomRunner.*:TwoSpeedRunner.*:NoComposition.*:FieldCaps.*:Workers.*:StrategyIcComposition.*:ThemeResid.*:ThemeTsmom.*:IcShrinkV1.*:IcShrinkAimV1.*:ThemeErcV1.*:MarginalIc.*` | 132 passed in 15 suites (StrategyIcRunner 51, MarginalIc 15, CompositionV8 13, ThemeResid 9, ThemeTsmom 7, StrategyIcComposition 7, IcShrinkV1 5, ThemeErcV1 5, FieldCaps 4, ThemeResidRunner 4, IcShrinkAimV1 4, NoComposition 3, ThemeTsmomRunner 2, TwoSpeedRunner 2, Workers 1), 54.6 s |
  | `atx-impl-strategy-target-tests --gtest_filter=TwoSpeed.*` | 20 passed |
  | S1's filter again after D1 (`MarginalIc.*:IcIdentity.*:CombineMarginalRankIc.*:` + the 2 pins) | 31 passed |
  | B1 after D1: `FactorsVerb.*` (target-tests) / `ResearchAdmission*` | 4 passed / 16 passed |
  | `atx-impl-tests --gtest_filter=CompositionRules.*:Exposures.*:FactorsVerb.*` (D1 and B1 through the glob; closes slot 6's deferred `Exposures.*`) | 14 passed (CompositionRules 6, Exposures 4, FactorsVerb 4) |

- **K-P9-6:** `atx-equity-strategy-ic.exe --list-rules --json` exit 0, output **JSON-equal** to
  `atx-impl/tests/fixtures/composition_rules_list.json` (8 rules); `--list-rules --json --workers 4` exits 2 (P7).
- **pytest** (touched: the fitter, `test_composition_resid.py`, the new plugin test; B1's comparator imports the
  fitter, so its suites re-ran too):

  | command | result | exit | wall |
  |---|---|---|---|
  | `-m pytest` on D1's ten fitter / composition suites (`atx-impl/tools/test_fit_composition_weights.py`, `_pool`, `_store`, `test_composition_ic_shrink.py`, `_resid`, `_rules`, `_theme_erc`, `_theme_tsmom`, `_two_speed`, `test_fit_composition_rule_plugins.py`) | **231 passed, 17 subtests** (= the lane's count) | 0 | 110 s |
  | `ATX_EQUITY_TARGETS_EXE`, `ATX_RESEARCH_ADMISSION_EXE` = post-D1 exes: `test_factor_series_admission.py test_exposures_export.py test_horizon_stats.py` | 20 passed, 0 skipped | 0 | 12 s |

- **Identity: X-5, flag absent** (D1 "How root verifies": R0-3's procedure with D1's exe and fitter). Argvs rebuilt
  token by token from R0-3's receipts (scratch `rerun.py`), only the listed tokens changed; R0-3's runs (v8-16d,
  source `d7c1c520`) are the references, X-5's own dirs the second reference; limits = R0-3's; source `1239a5ff`;
  compared by SHA-256 and JSON path only (scratch `dircmp.py`, `jdiff.py`, `wclass.py`); no value of a summary, daily
  IC or NAV file read.

  | pass | changed tokens | exe | outcome / wall / peak | vs R0-3 | vs X-5's own | match |
  |---|---|---|---|---|---|---|
  | fit (`p9-d1-x5-fit`) | `--output` | python, D1 fitter | completed 0 / 1.0 s / 58 MiB; "computed 0, reused 58" (D1's unchanged producer fingerprints) | `admission.csv` byte-identical; `admission.json` differs only at `/inputs/script_sha256`; `composition_weights.json` only at `/provenance/{admission_sha256, script_sha256, std/registry_sha256}` | same three files, same paths (= R0-3's PM7-30 set vs X-5) | **yes** |
  | w, cold cache (`p9-d1-x5-w`) | `--output`, `--candidate-cache` -> new empty `p9-d1-x5-cand-cache-empty` | ic `52892590` | completed 0 / 185.8 s / 2,401 MiB (cap 3,072; min free 1,276) | 10 / 12 files byte-identical (6 `train_combined.*`, daily IC, orientations, recipe, planned targets); `summary.json` 419 paths = 118 cache-directory paths (directory, fields_directory, 58 payload, 58 sidecar; each equal after substituting the cache dir) + 301 `*seconds` fields; `train_candidates.jsonl` 290 `*seconds` fields; I/O counters equal (both cold) | 10 / 12 identical | **yes** |
  | cache entries the cold run wrote | -- | -- | -- | 174 files, same set as R0-3's cache; 58 / 58 `.f64` payloads byte-identical; the 116 sidecars differ only at `engine_git_sha` (build provenance) | -- | yes |
  | NAV (`p9-d1-x5-nav`) | `--output` | targets `f0ee3908` (p9-1f, S1 + B1 + D1) | completed 0 / 54.1 s / 586 MiB | 27 / 27 byte-identical | 27 / 27 byte-identical; S2 daily `529062d6` = X-5's ledgered | **yes** |

  **Flag present** (D1: "every output file byte-identical except that summary.json gains theme_registry_sha256"):
  the w argv + `--theme-registry atx-impl/strategies/alphas/registry.json --theme-registry-sha256
  26b74e100cb34cf64c8868c292aff38805b89aef7ae5ac30a0ebac76da796ded`, on the cache the cold run had just filled
  (`p9-d1-x5-w-reg`): completed 0, 55.0 s, 1,371 MiB. Against X-5's own w (also warm): 10 / 12 identical;
  `summary.json` differs only at the 118 cache-directory paths and **`/theme_registry_sha256` (added)** (+ seconds);
  `train_candidates.jsonl` only seconds. Against my flag-absent cold run: the same added key, plus the warm-vs-cold
  cache counters (`hits`, `misses`, `vm_evaluations`, `field_loads`, `loaded_bytes`, `peak_resident_fields`,
  `verify_bytes`; `signal_cache` / `ic_result_cache` "miss" -> "hit") and seconds. **Match: yes.**

  **Result: D1 identity holds** (flag absent: fit, cold w and NAV reproduce X-5 / R0-3; flag present: only
  `theme_registry_sha256` added). These runs also re-check S1 (in the IC exe) and B1 (in the targets exe) at the
  wave-1 head before C1.
- **Slips fixed:** none in code (the shared-`endif()` hunk was a merge resolution, logged above).
- **Trial ledger:** 0 trials; `build-equity/trials.jsonl` 133 lines, sha256 prefix `27e40f9f`, mtime 11:32 (unchanged by every M1b run).

### M1b commits (first-parent, on `20443022`)

| commit | what |
|---|---|
| `7d618e24` | slot 5 (S1) closed: report section + log row + PM's pending `progress.md` lines |
| `1ab97798` | merge B1 (`01f20405`), 3 CMake list tails, all blocks kept |
| `394fc8b8`, `e6b583f2` | slot 6 log row / report section |
| `1239a5ff` | merge D1 (`757ba582`), 2 CMake list tails, all blocks kept (shared `endif()` resolved) |
| (next) | slot 7 + this hand-off, the log, `progress.md` |

### For M1c (slot 8, C1) and M1d

- **Status: M1b DONE.** Slots 5-7 merged, built, tested and identity-checked; no stop condition met. Known-red set
  untouched by my runs: none of the three known reds is in any filter or suite I ran, and no other failure occurred.
- **Next free build tag: `p9-1g`** (p9-1c = S1, p9-1d = B1, p9-1e = D1, p9-1f = B1 rebuilt after D1).
- **bin now** (equity-dev, source `1239a5ff`): ic `52892590`, targets `f0ee3908`, ic-tests `609f4005`, target-tests
  `fce04cd1`, impl-tests `f9eb7a2a`, admission `416966bc`, admission-tests `c2b35ce7`, fields exe `666ae58f` (p9-1b).
- **`atx-impl/CMakeLists.txt` tail** now ends with three blocks (S1 line; B1 `if()...endif()`; D1 `if()...endif()`).
  C1's append will conflict the same way: check that every `if()` keeps exactly one `endif()` after the resolution.
- **C1 stage-1 baseline:** `build-equity/p9-d1-x5-nav` is X-5's NAV at the wave-1 head minus C1 and is 27 / 27
  identical to X-5's ledgered dir and R0-3's `v8-i16d-x5-nav`; any of the three serves as "old X-5 NAV".
- **Reference exes kept** (with DLLs, runnable in place): `build-equity/p9-m1b-ref-ic` (v8-16d ic `985019d9`),
  `p9-m1b-ref-ic-s1` (S1-only ic `4e321143`), `p9-m1b-ref-targets` (v8-16d targets `72ff6d2d`).
- **Deferred to M1d:** S1's Release gtests (`MarginalIc.*`, S1's full filter, the whole IC test binary) and Release
  marginal adoption: no Release tree exists in pool-2; the first `equity-rel` configure fetches deps into the new
  `deps/equity-rel` (S1's preset change). G-P4 counts: D1's rules test is in `atx-impl-strategy-ic-tests` and
  `atx-impl-tests`; B1's admission tests are a new `atx_research` target -- count at M1d.
- **For E2 / D2** (B1, wave-2 carry): `signals.json` payload paths must be absolute or relative to the signals DIR
  (`strategy_factors_verb.cpp:117`); B1's comparator recipe as written produces repo-relative paths. A missing payload
  is reported as "extent" by the shared `load_pinned_f64` (minor: message conflates missing and wrong size).
- **Host:** free memory 3.2-4.8 GB at my launches, dipping to ~1.3 GB during the cold w (2.4 GB peak) while another
  session's processes ran. A pytest I did not start (PID 7524, `timeout.exe 1200 ... test_fit_composition_weights.py
  test_alpha_report_card.py`, from 15:23) was alive at hand-off; I left it. I left no process of my own.

## M1c: slot 8 (C1)

Agent root-m1c, 2026-10-03. Integration head at dispatch: `d656bbfa` (tree: the owner's png and other agents'
uncommitted `wave2-carry.md`, sprint plan, later `progress.md` lines, all left as they were until the docs commit).
Same Python and pytest flags as M1a/M1b; pytest only with explicit suite paths (PY-HYG); scratch logs `gt-*.log` /
`pyt-*.log` and the comparison scripts (`dircmp.py`, `jdiff.py`, `allowed.py`, `csvcols.py`, `navrun.py`) in the
session scratchpad `m1c/`.

### Slot 8: C1 (`10c35df3`, pool-15)

- **DEC-20 substitution list first:** written into `integration-log.md` ("M1c", "C1 NAV re-pin") and committed alone
  as **`02d6232b`** before the stage-1 build and before any C1 run. It is C1 report §4 (fix round 1) made exact: files
  and JSON paths per stage, labels L0-L4 / K, the Release clause and its plan §2.4 fallback.
- **Merge in two steps** (the bounded runner refuses a dirty code pathspec, so each stage needs a committed source):
  1. `git merge --no-ff dd925b7f` -> **`d71cabe1`** (sqrt only: `replay_cost.cpp`, `strategy_cost_v2.cpp`, their two
     test files). Stage-1 source.
  2. `git merge --no-ff 10c35df3` -> **`b52a5de7`** (the rest of the lane: NAV replay / v7 / risk-target sources,
     four test files, the lane report). Stage-2 source.
  - Both merged with no conflict. C1 edits no CMake file (its report: "No CMake ... edits"), so the hand-over's
    predicted `atx-impl/CMakeLists.txt` tail conflict did not arise; the file is unchanged by slot 8 (6 `if(` / 6
    `endif()` as M1b left it).
  - Tree check: `git diff 7ead85f7 b52a5de7` (`7ead85f7` = `git merge-tree --write-tree d656bbfa 10c35df3`, the
    single-merge tree) shows only my `integration-log.md` commit: the two-step merge gives the same code tree.
- **Pre-build reference:** `build-equity/bin/atx-equity-strategy-targets.exe` (`f0ee3908`, p9-1f, pre-C1, the exe
  of the stage-1 baseline `p9-d1-x5-nav`) copied with the 13 bin DLLs to `build-equity/p9-m1c-ref-targets-pre-c1/`.
- **Builds** (all equity-dev, `scripts/research-build.ps1`, 4 jobs; ConfiguredProvenance `1239a5ff...-dirty`: no
  reconfigure, C1 adds no file; DirtyEntries 3-4 = the files above, outside the code pathspec):

  | tag | source | targets | result | TUs / links | warnings |
  |---|---|---|---|---|---|
  | **p9-1g** | `d71cabe1` | `atx-equity-strategy-targets` (the stage-1 NAV exe only) | exit 0, 14.8 s, free 3,764 MiB | 2 / 3 | **0** |
  | **p9-1h** | `b52a5de7` | exactly C1's three: `atx-engine-w1-cost-tests, atx-impl-strategy-target-tests, atx-equity-strategy-targets` | exit 0, 113.9 s, free 3,871 MiB | 26 / 4 | **0** |
  | **p9-1i** | `b52a5de7` | earlier-lane targets that link `atx-impl-core` / `atx-engine` (C1's libraries): `atx-equity-strategy-ic, atx-impl-strategy-ic-tests, atx-impl-tests` | exit 0, 22.7 s, free 6,163 MiB | 12 / 4 | **0** |

  Logs: 0 "warning" lines, 0 "error" lines in each. C1's ~2,000 lines written without a compiler compiled first time
  under `/W4 /WX` (no slip). Executables: stage-1 targets
  **`5954565699f68d0430754c9e552a219fc2da0f29bb90d5baf354d2272de9452d`** (p9-1g, since overwritten in `bin`); p9-1h
  targets **`bf4b0ec25e77af6e6af8c67a7251337cbe24ce2ca48e8c86881c2f387c990f7a`**, target-tests `64da5532...01f4`,
  w1-cost-tests `ca235cb8...28cd`; p9-1i ic **`a1ceb9e038265ec90f0555565df855d1962467e58065a9c7591a83a02244735a`**,
  ic-tests `f970bf08...7a8c`, impl-tests `0de252d5...6a68`. The B1 admission and A2 fields targets link neither
  library (`atx::core` + json only), so they were not rebuilt.
- **gtests** (Debug, all exit 0, 0 failed; none of the three M1a-RED known reds is in any binary or filter run here):

  | binary / filter | result |
  |---|---|
  | `atx-engine-w1-cost-tests --gtest_filter=ReplayCostSqrt.*` (the Debug half of the sqrt probe) | 3 passed |
  | `atx-engine-w1-cost-tests` (whole binary) | 49 passed, 11 suites |
  | `atx-impl-strategy-target-tests` C1 new: `ReplayCostSqrt.*:NavBookRule.*:NavCapacityLockstep.*:NavSummaryBinding.*:VolTarget.TruncationInvariant:AdvHoldCapacityPerMultiple.*:SpoV3.CapacityDeclarationWithoutAdvHoldKeepsTheBaseBytes` | **13 passed** (NavBookRule 5, NavCapacityLockstep 3, and 1 each of the rest; C1-SPO's gtest, "written not run", passes) |
  | same exe, C1 regression `RiskTarget.*:VolTarget.*:NavV7Hook.*:CostV2*:AdvHold*:ConstructionGrid.*:NavBookWorkers.*:Spo*:TwoSpeed.*:NavLabelRole.*:InvVol.*:NavReplay*:Nav*` | **160 passed**, 31 suites (the filter misses `StrategyNavReplay`, `StrategyLive`, `TransferCoefficient`, ...; hence the next row) |
  | `atx-impl-strategy-target-tests` (whole binary: 59 suites incl. `StrategyNavReplay`, `StrategyLive`, B1 `FactorsVerb`, D1 `TwoSpeed`) | **338 passed**, 108 s |
  | earlier lanes on the p9-1i exes: S1 `MarginalIc.*:IcIdentity.*:CombineMarginalRankIc.*:` + 2 pins | 31 passed |
  | D1 new `CompositionRules.*:IcAdmission.*:ThemeRegistryRunner.*` | 8 passed |
  | D1 regressions (the 15-suite filter of slot 7) | 132 passed |
  | `atx-impl-tests --gtest_filter=CompositionRules.*:Exposures.*:FactorsVerb.*` (D1 / B1) | 14 passed |
  | `atx-impl-tests` with C1's new filter (the glob compiles C1's tests here too) | 13 passed |

- **K-P9-6 / K-P9-7 smoke:** `atx-equity-strategy-ic --list-rules --json` exit 0, JSON-equal to
  `atx-impl/tests/fixtures/composition_rules_list.json` after the relink; `atx-equity-strategy-targets nav
  --list-rules --json` exit 0, envelope `atx.nav-rules/v1` with `fixed-v1`, `vol-target-v1`, `risk-target-v1`
  (kind leverage).
- **pytest** (C1 touches no Python; the C1 reviewer's NAV-tools suites, and B1's exe suites because the targets exe
  changed):

  | command | result | exit | wall |
  |---|---|---|---|
  | `-m pytest -q -p no:cacheprovider atx-impl/tools/test_nav_summ.py atx-impl/tools/test_nav_summ_v8.py` | 29 passed, 1 skipped (= the review's count) | 0 | 11 s |
  | `ATX_EQUITY_TARGETS_EXE` = p9-1h, `ATX_RESEARCH_ADMISSION_EXE` = p9-1d: `test_factor_series_admission.py test_exposures_export.py test_horizon_stats.py` | 20 passed, 0 skipped | 0 | 13 s |

- **Identity: X-5 NAV, two Debug stages** (C1 report §4 run (a)). Every run: the argv of
  `build-equity/p9-d1-x5-nav-run/receipt.json` (X-5, L 1.1720, `--capacity-curve`, argv sha `7b517c04`) with only
  `--output` changed (scratch `navrun.py` asserts one changed position); `run_bounded_research.py` 600 s, cap 1,536
  MiB, floor 512, admission wait 900 s, `--build-type Debug`; receipts "clean in the code pathspec". Compared by
  SHA-256 per file (`dircmp.py`) and JSON leaf path (`jdiff.py`, classified by `allowed.py` against the pre-written
  list); for the CSVs sqrt moved, only header names and counts of differing cells (`csvcols.py`), no value.

  | run | source / exe | outcome | wall | peak RSS | min free |
  |---|---|---|---|---|---|
  | `p9-c1-s1-x5-nav(-run)` | `d71cabe1` / p9-1g `59545656` | completed, exit 0 | 113.7 s (host busy: another tree's compile) | 586 MiB | 2,954 MiB |
  | `p9-c1-s2-x5-nav(-run)` | `b52a5de7` / p9-1h `bf4b0ec2` | completed, exit 0 | 62.6 s | 591 MiB | 2,585 MiB |

  C1's admission risk ("+~110 MB" under `--max-bytes 1 GiB`) did not fire: X-5 was admitted, peak +5 MiB.

  **Stage 1** (sqrt only vs `p9-d1-x5-nav`, pre-C1 head):

  | list item | expected | observed | match |
  |---|---|---|---|
  | `daily_L0`, `events_L0`, `recipe.json`, `capacity/recipe.json` | byte-identical | identical | yes |
  | L1-L4 `daily_` / `events_` | may differ | 3 dailies differ (L1 `529062d6`->`75a54774`, L2 `0fc97711`->`a9a2e742`, L3 `53d9e446`->`93356e1e`); L4 daily and all 4 events identical | yes |
  | `capacity/` dailies / events | may differ | 5 dailies differ (x1 = L1 `75a54774`, still bit-equal to the primary); 5 events identical | yes |
  | `capacity_curve.csv`, `v7_transfer_coefficient.csv` | may differ | identical | yes |
  | `summary.json` (`a03937cf`->`5d3fab71`) | only `/scenarios/1..4`, `/warm_start/...<L1..L4>`, `/v7/books/<L1..L4>` | 4 leaves: `/scenarios/{1,2,3}/daily_csv_sha256`, `/scenarios/3/costs/unrationed_full_request_cost_dollars`; 0 outside the list | yes |
  | `capacity/summary.json` (`1d93afb8`->`1163a2a5`) | only `/scenarios/0..4`, `/warm_start/...<K>`, `/v7/books/<L1..L4>` | 5 leaves `/scenarios/{0..4}/daily_csv_sha256`; 0 outside | yes |
  | `v7_extras.json` | only `/files/{TC, curve}`, `/capacity/0..4` | identical | yes |
  | files on one side only | none | none (27 / 27) | yes |

  Columns sqrt moved (header names only): `impact_cost_dollars`, `unrationed_cost_dollars`, `trade_cost_dollars`,
  `trade_cost_return`, 1-3 cells per file in 1,005 rows; no NAV / return / position column moved. **Stage 1 holds.**

  **Stage 2** (whole lane vs stage 1):

  | list item | expected | observed | match |
  |---|---|---|---|
  | 20 CSVs, both recipes, `capacity/summary.json`, `capacity_curve.csv`, `v7_transfer_coefficient.csv` | byte-identical | 25 / 25 identical | **yes** |
  | `summary.json` (`5d3fab71`->`5c34142b`) | only `/v7/extras`, `/v7/files/*` (added), `/producer/*` (added) | CHANGED `/v7/extras`; ADDED `/v7/files/{v7_extras.json, capacity/summary.json}`, `/producer/{build_type, definition, engine_git_sha}`; 0 outside | **yes** |
  | `v7_extras.json` (`3bd003f2`->`474ea2bf`) | only `/files/capacity/summary.json` (added) | that one leaf added; 0 outside | **yes** |

  Binding check (SHA strings, not statistics): `summary.json` `/v7/files` = the SHA-256 of `v7_extras.json` and of
  `capacity/summary.json` on disk; `v7_extras.json` `/files` = the SHA-256 of `capacity/summary.json`, the TC CSV and
  the curve; `capacity_x1_equals_primary_bit_for_bit` true and the two files equal; `capacity/summary.json` has no
  `producer` and pass `capacity`; mtimes recipe < capacity summary < extras < summary (summary last).
  `producer` = `{build_type: debug, engine_git_sha: 1239a5ff0fd31689651d79a4c0844173cabd4c06-dirty, definition}`:
  the configure-time commit, by its own definition (the tree was last configured at `1239a5ff`; C1 adds no file, so no
  build reconfigured). **Stage 2 holds.**

  **Re-pin** (DEC-20, ruled list): the new Debug X-5 NAV reference is `build-equity/p9-c1-s2-x5-nav` (S2 primary daily
  `75a54774b417...`, was `529062d6`); every move is inside the pre-written list.
- **Slips fixed:** none (C1 compiled first time; no CMake conflict).
- **Trial ledger:** 0 trials; `build-equity/trials.jsonl` 133 lines, sha256 prefix `27e40f9f`, mtime 11:32 (unchanged).
- **Disclosure:** while listing `summary.json`'s key skeleton of the pre-C1 baseline I printed, by a key filter that
  matched `financing`, the `scenarios[*].financing` blocks (TRAIN 2020-2023 financing dollars / shares) of
  `p9-d1-x5-nav` and its `capacity/summary.json`. Nothing dated 2024-01-01 or later; no comparison or decision used
  them (identity is SHA and path only); every later print was paths, SHAs or header names.

### Slot 8 Release clause (C1 report §4 run (b), G-P3 NAV half)

The Debug results above were committed first (`61ac4423`). Then:

- **Memory gate:** `research-build.ps1 -Preset equity-rel -DryRun` -> Admitted True (free 5,648 MiB, commit 8,240
  MiB); no wait was needed.
- **Configure** (first Release tree in pool-2): `scripts\atx-build.ps1 configure -Preset equity-rel` -> exit 0,
  121 s; `build-equity-rel/` (gitignored `build-*/`), deps fetched into `deps/equity-rel` (S1's preset), vcpkg
  packages already installed; engine_git_sha `61ac4423...-dirty` (configure-time HEAD; code = `b52a5de7`). The one
  `NativeCommandError` line in the log is PowerShell wrapping cmake's stderr notice `# date: USE_SYSTEM_TZ_DB OFF`.
- **Build p9-1j** (equity-rel, source `61ac4423` = `b52a5de7` + docs, DirtyEntries 4 outside the code pathspec, 4
  jobs, free 4,896 MiB): exactly C1's three targets -> **exit 0, 802.6 s, 314 TUs (from scratch), 11 links**.
  Warnings: **0 first-party**; the log has 11 "warning" lines, all third-party: 7 x `clang-cl: warning: argument
  unused during compilation: '/MP'` on the FetchContent spdlog TUs (`deps/equity-rel/spdlog-build`), and 4 lines from
  the vendored `atx-core/third-party/databento-cpp` (`getenv` deprecated, `historical.cpp:1142`, `live.cpp:18`, plus
  two "1 warning generated"). First-party code builds under `/W4 /WX`, so any first-party warning would have failed
  the build. Executables: targets **`93ea323ea02817ef8c7b6c84f304332718bc7b3ed6e46a6c61f7a96dc5b266cc`**,
  target-tests `671d9e44...bd01`, w1-cost-tests `51011f9e...ec31`.
- **gtests, Release:**

  | binary / filter | result |
  |---|---|
  | `atx-engine-w1-cost-tests --gtest_filter=ReplayCostSqrt.*` (the Release half of the sqrt probe) | 3 passed |
  | `atx-engine-w1-cost-tests` (whole) | 49 passed |
  | `atx-impl-strategy-target-tests` C1 new filter (includes `ReplayCostSqrt.CapacityLawAndS2MarginalCostBitsArePinned`) | 13 passed |
  | `atx-impl-strategy-target-tests` (whole) | **336 passed, 2 FAILED**: `BookNormalScore.TiesShareTheMeanRankAndMirrorsAreOpposite`, `BookNormalScore.FixtureTellsWrongRulesApart` |

  **The two Release failures are not from C1 and predate wave 1.** Both are `EXPECT_EQ` of
  `engine::book::normal_scores` output against `eval::norm_ppf(u)` that misses by 1 ULP in Release only
  (`book_normal_score_test.cpp:57`: `out[4]` 0.96742156610170082 vs `norm_ppf(10/12)` ...071; `:76`: `top`
  3.0908256489420887 vs `norm_ppf(1001/1002)` ...891). The kernel is header-only (`normal_score.hpp`), from v8 commit
  `02633038` (Y-3 norm-score-v1); `git diff d7c1c520 HEAD` is empty for `normal_score.hpp`, the test and every
  `norm_ppf` header (`deflated_sharpe.hpp`, `decay_monitor.hpp`), and C1 touches none of them. The same binary in Debug
  passes 338/338. No Release build of this test existed before (pool-2 had no Release tree). Not a known red under
  M1a-RED, so the M1a-RED gate ("failure set == exactly the three") needs a ruling. I fixed nothing and edited no
  expectation. Exposure: only `--rank-shape norm-score-v1` books (the Y-3 template) call this kernel. X-5 has no rank
  shape, so the NAV identity below does not cover it.
- **Identity: X-5 NAV, Release vs stage-2 Debug.** Same argv; only the exe path and `--output` changed (`navrun.py`:
  positions 0 and 15); `--build-type Release`; same limits.

  | run | exe | outcome | wall | peak RSS | min free |
  |---|---|---|---|---|---|
  | `p9-c1-rel-x5-nav(-run)` | p9-1j `93ea323e` | completed, exit 0 | 41.7 s (Debug: 62.6 s) | 581 MiB | 2,220 MiB |

  | compared with `p9-c1-s2-x5-nav` | expected | observed | match |
  |---|---|---|---|
  | 26 files other than `summary.json` (all CSVs, both recipes, `capacity/summary.json`, `v7_extras.json`) | byte-identical | 26 / 26 identical | **yes** |
  | `summary.json` (`5c34142b` -> `42581a3d`) | only `/producer` | CHANGED `/producer/build_type` (debug -> release), `/producer/engine_git_sha` (`1239a5ff...-dirty` -> `61ac4423...-dirty`, the two trees' configure commits); 0 other leaves; the object minus `producer` is equal | **yes** |

  **Release clause holds:** with `sqrt` in, the Release NAV of X-5 equals Debug bit for bit outside `producer`. The
  plan §2.4 C1 fallback (NAV stays Debug) is not triggered, and G-P3's NAV half is shown on X-5 at this build. Neither
  `guarded_move`'s `std::log` nor the p95 / cagr CRT calls moved a byte on this book set.
- **Trial ledger:** 0 trials; 133 lines, `27e40f9f` (re-checked after the Release run).

### M1c commits (first-parent, on `d656bbfa`)

| commit | what |
|---|---|
| `02d6232b` | DEC-20 substitution list in `integration-log.md`, before any C1 run |
| `d71cabe1` | merge C1 step 1/2: `dd925b7f` (sqrt), the stage-1 source |
| `b52a5de7` | merge C1 step 2/2: `10c35df3` (whole lane), the stage-2 source; code tree = single-merge tree `7ead85f7` |
| `61ac4423` | Debug results (report section, log row) |
| (next) | Release clause + this hand-off, the log, `progress.md` (PM's pending lines unedited + `- M1c result:`) |
| (next) | PM / doc files as they were in the tree: sprint plan, `wave2-carry.md`, `wave2-lane-dispatch.md` |

### For M1d (post-merge gates) and the whole-wave review

- **Status: M1c DONE, with one item for a PM ruling:** the Release-only `BookNormalScore` pair above (v8 code, not a
  merge result). Until it is ruled, a Release run of `atx-impl-strategy-target-tests` (or a Release `ctest` over it)
  fails exactly these two, besides the three M1a-RED known reds. C1's slot found no slip, no design error and no
  identity difference outside the written list.
- **Next free build tag: `p9-1k`** (p9-1g stage-1 exe, p9-1h C1 Debug, p9-1i earlier lanes after C1, p9-1j C1 Release).
- **Wave-1 code head = `b52a5de7`** (all 8 slots; my later commits are docs only).
- **bin now (equity-dev, Debug):** targets `bf4b0ec2`, target-tests `64da5532`, w1-cost-tests `ca235cb8`, ic
  `a1ceb9e0`, ic-tests `f970bf08`, impl-tests `0de252d5`; unchanged from M1b: admission `416966bc`, admission-tests
  `c2b35ce7`, fields exe `666ae58f`.
- **Release tree now exists:** `build-equity-rel/` (configured at `61ac4423`, deps in `deps/equity-rel`), holding
  Release targets `93ea323e`, target-tests `671d9e44`, w1-cost-tests `51011f9e`. M1d's Release IC adoption (S1's
  deferred Release gtests and marginal) and the Release canary build on it. Expect it to compile only the IC closure
  now. A from-scratch Release build cost 803 s at 4 jobs.
- **Debug configure provenance is stale:** `build-equity` was last configured at `1239a5ff` (no CMake change since),
  so a Debug NAV `summary.json` `/producer/engine_git_sha` reads `1239a5ff...-dirty`. That matches the field's own
  definition (configure-time commit), but it does not name the code head. If P9-B0's re-based reference or the canary
  goldens should carry the current commit, reconfigure the Debug tree first (`atx-build.ps1 configure -Preset
  equity-dev`). `producer` is dropped from every cross-build comparison and is on the DEC-20 list either way.
- **P9-B0 (DEC-20), C1 part:** the NAV substitution list for any parent NAV re-run under this build is the union of my
  stage-1 and stage-2 lists (integration log "M1c"). Sqrt may move the S2-family / capacity CSVs and the matching
  `summary.json` / `capacity/summary.json` / `v7_extras.json` paths. Structure adds `/v7/files`, `/producer`,
  `/v7/extras` (text) and `v7_extras` `/files/capacity/summary.json`. On X-5 the observed sqrt move is small: 8
  dailies, `daily_csv_sha256` leaves and 1 cost leaf, with no events file, curve or TC file. The new Debug X-5 NAV
  reference is `build-equity/p9-c1-s2-x5-nav` (S2 daily `75a54774`); the Release twin is `p9-c1-rel-x5-nav`.
- **Canary goldens (G-P8):** T1's goldens are recorded after C1, which is now merged. Record them on equity-dev, then
  equity-rel (`research-build.ps1 -Canary` / `-CanaryRecord`; `--record` needs the re-pin flag). The tag must build both
  canary exes (ic + targets).
- **Reference exes kept** (with DLLs): `build-equity/p9-m1c-ref-targets-pre-c1` (`f0ee3908`, p9-1f, pre-C1), plus
  M1b's three.
- **Host:** free memory 2.2-6.2 GB during my runs; another tree's build (pool-21) and pytest ran alongside. I left no
  process of my own (checked at hand-off).

## M1d: post-merge gates (STOPPED by the owner, partial)

Agent root-m1d, 2026-10-03, from `ad406718` (code `b52a5de7`). Stopped on the owner's request after block 1's gtest
half; full state in `handoff-root-m1d.md` (same dir). No code commit, no merge, no expected hash touched; 0 trials
(ledger 133 lines, `27e40f9f`).

- **Debug reconfigure** (`atx-build.ps1 configure -Preset equity-dev`): exit 0, 37 s; `engine_git_sha`
  `ad406718...-dirty` (was `1239a5ff...-dirty`).
- **Build p9-1k** (equity-dev, 4 jobs, free 4,229 MiB): 14 targets (ic, targets, ic-tests, target-tests, impl-tests,
  strategy-mine-tests, strategy-tests, engine alpha / factory / book tests, research-fields(-tests),
  research-admission(-tests)) -> exit 0, 193.3 s, 36 TUs, 14 links, **0 warnings**. Exes: ic `393183c0`, targets
  `407c34ad`, ic-tests `13d7b053`, target-tests `d08a739a`, impl-tests `dd58e3ad`, mine-tests `336e1b67`,
  strategy-tests `e1b27211`, alpha `723508b1`, factory `d2c1284d`, book `1436bf72`, fields-tests `1b364bf8`.

  | suite (Debug, p9-1k) | result |
  |---|---|
  | `ctest -L atx_research` (106 = fields 46 + admission 16 + mine 44) | 104 passed, 2 failed = M1a-RED's two gtests, assertion text identical to M1a's record |
  | `ctest -L atx_equity_strategy` (577 = strategy-tests 46 + ic-tests 193 + target-tests 338) | 577 passed |
  | `atx-engine-alpha-tests` / `-factory-tests` / `-book-tests` (whole) | 771 / 392 / 184 passed |
  | golden `0x889874a3b9b29c55` (`SignalFitnessDefaults.*` workers 1 and 4; `NsgaSearch.ScalarRaw_*` 1/2/4/8) | holds |
  | `atx-impl-tests` (whole) | not run (stop) |
  | pytest (scripts/tests x2 seeds, engine/tools, impl/tools, impl/strategies) | killed mid scripts/tests seed 0 at the stop; no result |

- Not started: canary goldens (both still null), Release IC adoption, P9-B0 (no substitution list written), scoreboard /
  timings, G-P ticks. Next free build tag **p9-1l**.

### M1d resume (root-m1d resume, 2026-10-03 evening)

Resumed from `handoff-root-m1d.md` at head `3c5f0b8e` (resume rulings); nothing done in the stopped run was redone.
Code is still the wave-1 head `b52a5de7` (every later commit is docs or the goldens file). Debug bin = p9-1k (checked by
SHA against its receipt). Scratch logs and scripts: session scratchpad `m1d/`. Host: lanes compiling most of the time;
free memory 1.2-4.6 GiB; the bounded runner's admission (no compiler running) made P9-B0 / adoption runs wait up to
9.3 min.

**Status: STOP for a PM ruling.** Every block is done; P9-B0 holds; but the wave-1 failure set is NOT within the five
named known-reds: 1 Debug + 7 Release failures more (below). No code was changed, no expected hash edited.

#### Block 1 (rest): `atx-impl-tests` whole and pytest

| suite | result | wall |
|---|---|---|
| `atx-impl-tests` (Debug, p9-1k `dd58e3ad`, whole binary, first whole run in P9) | 1,130 tests: 1,124 passed, 5 skipped, **1 FAILED: `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest`** | 1,256 s |
| pytest `scripts/tests`, `PYTHONHASHSEED=0` | 373 passed, 4 skipped, 1 failed = M1a-RED `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` (`:119`, the same three names `noa_lag4`, `ea_window_pre5`, `ins_cluster_buy`) | 1,136 s |
| pytest `scripts/tests`, `PYTHONHASHSEED=1` | 373 passed, 4 skipped, 1 failed (the same) | 1,074 s |
| pytest `atx-engine/tools` | 399 passed, 6 subtests | 647 s |
| pytest `atx-impl/tools` | 640 passed, 3 skipped, 17 subtests | 658 s |
| pytest `atx-impl/strategies` | 14 passed | 10 s |

All pytest: explicit paths, `-p no:cacheprovider`, `--basetemp` in the scratchpad (deleted after), `PYTHONDONTWRITEBYTECODE=1`;
`scripts/tests` ran after both canary goldens were committed. `test_no_versioned_scripts.py` (G-P6) and
`test_no_python_mirror.py` (G-P5 guard, 37 mirrored rule functions in 14 rows) green under both seeds.

**New red 1 (Debug, pre-existing, not from wave 1): `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest`**
(`atx-impl/tests/provenance_digest_test.cpp:373`): `_manifest.txt` differs between two discover runs that differ only
in `config_json` (`field_cardinality_max` 12 vs 999), because the manifest now carries a `config_json=...` line.
That line was added by `d060cd81` (2026-09-26, "bind IC screening configuration and discovery resume identity",
`atx-impl/src/stage_discover.cpp:963, 1444`); the test's assertion (manifest invariant to config_json) dates from
`4c795fbe` (2026-06-28). `git diff 6a68d7f9 HEAD` on `stage_discover.cpp` and the test is empty: it has been red since
2026-09-26, before the P9 base; no wave-1 lane touches it. Nobody ran this binary whole before (M1b / M1c ran filters).
Not fixed: whether the manifest or the test is the contract is a ruling.

#### Block 2: tiny-world canary goldens (G-P8)

| tag | preset | source | build | canary | result |
|---|---|---|---|---|---|
| **p9-1l** | equity-dev | `7d5b2ec9` | ic + targets, 0 TUs, 8.4 s | `-CanaryRecord` (first record, no `--repin`: both entries were null) | exit 0, 24.2 s; Debug goldens written, committed `506ea123` |
| **p9-1m** | equity-dev | `506ea123` | 0 TUs, 8.7 s | `-Canary` (check) | **15 passed**, exit 0 |
| **p9-1n** | equity-rel | `506ea123` | ic, targets, ic-tests, w1-foundation-tests: **25 TUs, 3 links, 232.8 s, 0 warnings** | `-CanaryRecord` | exit 0, 16.5 s; Release goldens committed `5e1ebbbc` |
| **p9-1o** | equity-rel | `5e1ebbbc` | 0 TUs, 15.6 s | `-Canary` (check) | **15 passed**, exit 0 |

Goldens (`scripts/tests/fixtures/tiny_world_goldens.json`): the Release entry equals the Debug entry on all five
digests: u orientations `38115194`, fit admission decisions `71b47e87`, fit weights `d222c876`, w combined f64
`1f7ac0a8`, NAV primary daily `eb385a15` (primary `modeled-1bn-stale5-v1+swap-fin-v1`; planted members within the
T1-SE band in both). Exes: Debug ic `393183c0` / targets `407c34ad`; Release ic **`ea370cec`** / targets `93ea323e`
(p9-1j's, unchanged). First-run seconds 19.6 (Debug record, host busy) / 12.6 (Release); both checks passed the 15 s
budget. Release tree not reconfigured (configured at `61ac4423`, code `b52a5de7`).

#### Block 3: Release IC adoption (G-P3, IC half)

Expectation written and committed before the runs (`7d5b2ec9`, integration log). Release exes p9-1n: ic-tests
`0748dd5c`, w1-foundation-tests `daf0009c`.

| Release gtests (p9-1n) | result |
|---|---|
| `atx-impl-strategy-ic-tests` S1 filter (`MarginalIc.*:IcIdentity.*:CombineMarginalRankIc.*` + 2 pins) | **31 passed** |
| `atx-engine-w1-foundation-tests` whole (alpha oracle, conformance, widened conformance, streaming, combine) | **83 passed** |
| `atx-impl-strategy-ic-tests` whole | **186 passed, 7 FAILED** (Debug: 193 / 193) |

**New red 2 (Release only, from S1's Release cache root): 7 `StrategyIcRunner.*`**
(`ExtraFieldsResolveByNameAndMatchHandComputedSignals`, `GrpFieldsBindAsGroupClassifiersAndExcludeNaNLabels`,
`CandidateCacheKeyChangesOnlyForCandidatesReadingAChangedFieldPayload`, `CandidateCacheReadsV1EntriesInPlaceThroughKnownManifests`,
`CandidateCacheWritesFitWithinTheDeepestCommittedPath`, `ExtraFieldsResideOnlyWhileReferencedAndReloadExactly`,
`ExtraFieldsAreMaskedByRolePresence`), each `IoError: IC runner: candidate cache partial output: <path>`
(`strategy_ic_signal_cache.cpp:416`). Every failing path is **262 characters**: the test scratch root
(`%TEMP%\atxv-<id>\atx-strategy-ic-runner-<ts>-<n>\c\`, 94 chars) + S1's Release identity directory
`dslvm1_clang18.1_opt_md_ndebug_xs13.0.0\` (40 chars; the Debug root has none) + role / `fp_` / `ic1_` dirs + the
`.<16 hex>.partial` file: over Windows MAX_PATH (260). Debug paths are 222. S1's lane never built (no Release run of
these tests existed; M1b deferred them to M1d). Real-data Release runs are under the limit (my DIR: longest 233; the
spec cache `mega-candidate-cache-v8-lo3` + Release root: ~244 for the longest entry name), but the margin is ~16
characters. Not a slip (not a typo / include); not fixed: the fix is a layout or test-scratch decision (shorter Release
root token, shorter test scratch, or long-path support), for the PM.

Runs (DIR = `build-equity/p9-m1d-rel-cache`, hard-link copy of `p9-d1-x5-cand-cache-empty`; argvs from the named
receipts, only `--candidate-cache`, `--output` and the exe changed; `relrun.py`; Release PATH = vcpkg release bin only):

| step | run | outcome | wall / peak |
|---|---|---|---|
| 1 | Debug marginal (`p9-s1-x5-marginal-new-run` argv) | first try `p9-m1d-dbg-marginal-run`: **time-limit** at the reference's 360 s (host busy; the reference took 178 s); rerun `p9-m1d-dbg-marginal2` at 600 s (the runner's maximum): completed | 416.5 s / 297 MiB |
| 2 | Release marginal, same argv, before any Release u pass | **refused as specified**: exit 1, 0.3 s, `NotFound: marginal IC: no candidate cache entry for value_composite (...) on role e1c67101...; run the u pass with --candidate-cache first`; no output dir | 0.3 s |
| 3 | Debug u (`mega-v8-b0b-train-u-v8x3b-run1` argv) | completed (warm Debug) | 92.3 s / 673 MiB |
| 4 | Release u, same argv (cap 3,072 for the cold pass) | completed; wrote `DIR/dslvm1_clang18.1_opt_md_ndebug_xs13.0.0/` | 160.5 s / 1,693 MiB |
| 5 | Debug w (`p9-d1-x5-w-run` argv) | completed (warm) | 87.2 s / 1,371 MiB |
| 6 | Release w, same argv | completed (warm Release) | 52.6 s / 1,362 MiB |
| 7 | Release marginal, as step 2 | **completed** | 104.8 s / 295 MiB |

| Debug vs Release | payload files | other files (paths only) |
|---|---|---|
| u (3 vs 4) | **10 / 10 byte-identical**: orientations `3428b2f8` (= X-5 spec pin `reference_orientations`), daily IC `51b0f8d4` (= pin `reference_daily_ic`), recipe, 6 `train_combined.*`, planned targets | `summary.json` 666 leaves: identity strings (`vm_identity` -> `..._opt_md_ndebug_xs13.0.0`, `ic_results` identity / key / subdirectory, 58 `signal_key_sha256`), cache-root paths (directory, fields_directory, 58 payload + 58 sidecar paths now under `DIR/<Release root>/`, same file names), cache state counters (hits, misses, vm_evaluations, ic_results hits / misses, research_fields loads / bytes / peak, verify_bytes, ic_cache_bytes, ic_scratch_bytes), timing. `train_candidates.jsonl`: timing + `signal_cache` / `ic_result_cache` "hit" -> "miss" (58 each): warm Debug vs cold Release, cache state, not a payload; my written jsonl expectation named timing only, so this is a recorded deviation, explained by step 5 vs 6 below |
| w (5 vs 6) | **10 / 10 byte-identical** (combined json `2a442f56` = X-5 spec pin `reference_combined`) | `summary.json`: identity strings, cache-root paths, `ic_cache_bytes`, `ic_scratch_bytes`, timing; `train_candidates.jsonl`: **timing only** (both warm: hit / hit) |
| marginal (1b vs 7) | `marginal_ic.json` | **only** `/inputs/candidate_cache/build_vm_identity` and `/stage_seconds/*` (7): exactly the expectation. (Debug on DIR vs S1's Debug on the shared cache: only `/inputs/candidate_cache/directory` + `/stage_seconds/*`.) |

**Release IC adoption on real data holds** (u, w, marginal: payloads bit-identical to Debug; the NotFound refusal and
the fill-then-succeed behaviour exactly as S1 specified). Speed (S1 item 5, logged): marginal 104.8 s Release vs
416.5 s Debug on a busy host (178 s Debug when quieter); w 52.6 s vs 87.2 s; cold Release u 160.5 s.
G-P3 IC half is **not ticked** while new red 2 (Release `atx-impl-strategy-ic-tests`) is unruled.

#### Block 4: P9-B0 (DEC-20, Ruling B0-Y1)

Substitution list written and committed alone before any run: **`7d5b2ec9`** (integration log, "P9-B0 substitution
list"): runs, per-file / per-path lists, and the B0-Y1 code-path proof (7 steps) for the Y-1 `vol_target` rows and
L1-L4 book blocks. Results (integration log "P9-B0 result", `f9fbae2f`):

| run | vs reference | outside the list |
|---|---|---|
| u `p9-b0-yf0-u` | 3 files identical; `summary.json` 140 + `train_candidates.jsonl` 134 timing leaves | 0 |
| fit `p9-b0-yf0-fit` | `admission.csv` identical; `admission.json` `/inputs/script_sha256`; `composition_weights.json` `/provenance/{script_sha256, admission_sha256}`; `std/registry_sha256` identical as predicted | 0 |
| w `p9-b0-yf0-w` | 10 payload files identical (combined json `e13fbc4d`); `summary.json` 277 + jsonl 268 timing leaves | 0 |
| NAV Y-F0 `p9-b0-yf0-nav` | 16 identical; 8 sqrt CSVs (cost columns only, 1-8 rows of 1,006); summary `/scenarios/1..4`, `/v7/extras`, `/v7/files` +2, `/producer` +3; capacity summary `/scenarios/1..4`; extras `/files/capacity/summary.json` +1 | 0 |
| NAV X-10 `p9-b0-x10-nav` | 15 identical; 9 sqrt CSVs; same JSON paths (capacity `/scenarios/0..4`) | 0 |
| NAV Y-1 `p9-b0-y1-nav` | 17 identical incl. **`vol_target.csv`** (`100286a5`); 8 sqrt CSVs; same JSON paths; no `/vol_target` leaf moved (B0-Y1 additions allowed, not needed) | 0 |

**P9-B0 holds.** Re-based references: the six `build-equity/p9-b0-*` dirs (Y-F0 S2 daily `e7eb5720`, was `73b69bcc`;
summaries Y-F0 `4f048210`, X-10 `42d104fc`, Y-1 `07e23261`). No statistic was read: SHA, JSON paths, CSV header
names and differing-row counts only.

#### Block 5: scoreboard, ledger, timings

`research_cycle.py scoreboard --timings` exit 0 (2 s): lineage `(parent) x-theme-erc-gm.json` -> `y-s
lib-v8ysb-gm.json`; 4 waves (y-s ACCEPTED; y-2, y-3, y-5 NOT ACCEPTED); checks 4 / 4 (each trial ledgered, its
`s2_net_sr` equal); ledger `build-equity/trials.jsonl` chain verified, 133 lines, head `5a3ef9d9`, N 62. Timings table
appended to the integration log. Disclosure: the scoreboard prints its ledgered TRAIN (2020-2023) rows by design; I
saw them on the console; no decision here uses them and none is copied into these files.

Trial ledger: **0 trials**; 133 lines, sha256 `27e40f9f`, mtime 11:32 (unchanged at the end).

#### Block 6: G-P rows (detail in the integration log)

Ticked: **G-P4** (CTest registration; atx_research 106, atx_equity_strategy 577), **G-P6** (guard green under both
seeds; class-C generators deleted at M1a), **G-P8** (canary goldens recorded and checked on Debug and Release; Release
= Debug digests). Shown but **not ticked**: G-P3 IC half (runs hold; Release ic-tests new red 2 unruled). Not wave-1
checkable: G-P1, G-P2, G-P5 (guard only), G-P7 (first half only), G-P9, G-P3 NAV half (shown on X-5 at M1c; wave 3).

#### Failure set at the end of M1d

| failure | build | named known-red? |
|---|---|---|
| `test_research_mine.py::test_fields_are_the_rule_applied_to_the_registry` | both seeds | yes (M1a-RED) |
| `ResearchFieldsWriter.QuantilesPartitionLikeNumpy`, `ResearchFieldsVolumeMean.SumOrderIsNumpys` | Debug (blocks of the stopped run) | yes (M1a-RED) |
| `BookNormalScore.TiesShareTheMeanRankAndMirrorsAreOpposite`, `BookNormalScore.FixtureTellsWrongRulesApart` | Release (M1c run; not re-run: no Release target-tests rebuild in this resume) | yes (M1c-RED) |
| `AtxImplProvenanceDigest.ConfigJsonNotInDiscoverDigest` | Debug `atx-impl-tests` | **no** (new red 1; pre-existing since `d060cd81`) |
| 7 x `StrategyIcRunner.*` (MAX_PATH) | Release `atx-impl-strategy-ic-tests` | **no** (new red 2; S1 Release cache root) |

Gate "failure set within the five named known-reds": **not met** -> STOP for a ruling on new reds 1 and 2.

#### Disk (Rulings DISK, DISK-2)

Deleted after every gate and P9-B0 had finished (rename probe, then remove; each checked: 0 tracked files,
git-ignored, no reparse point inside): my `p9-m1d-rel-cache` (348 files: the hard links + the Release root);
DISK-2's `p9-d1-x5-cand-cache-empty` and `v8-i16d-cand-cache-empty` (174 files each; no P9 spec, test, plan or
sprint brief names them, only receipts, logs and the disk audit); the 27 `.f64` payloads of
`recent-fast-validation-2023-2024-v1-fields-v{1,2,3}` (11 + 8 + 8, 984,602,304 B; each `manifest.json` kept; files
deleted unread). Free space on C: 36,422,791,168 B before, 49,014,763,520 B after (other agents wrote meanwhile).
Also my five pytest basetemp dirs (their `*current` symlinks pointed inside the basetemp) and the one `.pyc` my
canary run wrote (`atx-impl/tools/__pycache__/fit_composition_weights.cpython-312.pyc`). Kept:
every P9-B0 / adoption run dir (evidence, ~380 MiB), all pinned caches, references and `trials.jsonl`.

#### M1d resume commits (first-parent, on `3c5f0b8e`)

| commit | what |
|---|---|
| `7d5b2ec9` | P9-B0 substitution list (B0-Y1) and the Release IC expectation, before any run |
| `506ea123` | tiny-world goldens, Debug first record (p9-1l) |
| `5e1ebbbc` | tiny-world goldens, Release first record (p9-1n) |
| `f9fbae2f` | P9-B0 result in the integration log |
| (next) | this section, the integration-log close-out (G-P ticks, timings, disk) |

Next free build tag: **p9-1p**. No process of mine left running (checked).
