# P9 lane briefs (paste one section, plus "Rules for every P9 lane", into an Opus 5.5 implementer's dispatch)

Plan: `docs/plans/2026-10-03-p9-sprint-plan.md` (cited as "plan §n"). Finding ids (F-n, P9-Rn, NV-n, OR-n, FD-n, CM-n,
DS-n) and contracts (K-P9-n) are defined in plan §0 and §2.3. Literature ids (lit §n, [n], F1..F18) refer to
`docs/plans/2026-10-02-p9-literature-review.md`. Root fills `<frozen-sha>` and the pool at dispatch.

## Rules for every P9 lane

1. Read first: `.superpowers/sdd/platform-v8-20260929/lane-rules.md` (binding: never build C++, never run real data,
   never dispatch subagents, never push, never touch `atx-db/`, files only with the Write / Edit tools because the
   shell hook breaks heredocs), then plan §0.6 and §2.2-§2.3, then the review files your brief names. For C++:
   `.agents/cpp/agent.md` first; write code that compiles first time under clang-cl 18 `/W4 /permissive- /WX`
   (no unused variables, sign conversions or shadowing; 100-column limit; copy the owning file's idiom).
2. Work only in your leased pool on your branch (`feat/p9-<id>-20261003`, base `<frozen-sha>`). Lease:
   `powershell scripts\lease-worktree.ps1 -Branch feat/p9-<id>-20261003 -Base <frozen-sha> -Agent p9-<id>
   -RunId p9-<id>-20261003 -HeartbeatId p9-<id>-hb -MaxPool 20` (root may have leased it for you; check `-Status`).
3. Blind. Do not open any return, IC, Sharpe, turnover or NAV output of 2020-2023 (`build-equity/` NAV, cards,
   marginal, admission and diagnostics files are closed; manifests, receipts and field lists are open). Nothing dated
   2024-01-01 or later is opened by you or by code you run. The numbers in status 7 and the ledger are public.
4. Identity discipline: every change is behind a flag or provably value-preserving; flag absent = byte-identical; say
   exactly how root verifies it (targets, gtest filters, argv, the expected byte-identical files, any substitution
   list). Never edit an expected hash. A Python copy of a C++ rule is deleted only in a later slice, after root's
   identity run (plan §0.6).
5. PM8-12: numerical and research logic goes in atx-engine C++ (generic) or atx-impl C++ (strategy-specific) with
   gtests; Python is orchestration, specs, receipts and reports. No new versioned copy of any script; no new
   `research_fields_*.py` builder module (plan DEC-5).
6. Stay inside "Files in scope". Touching a file another lane owns is a lane failure unless the brief names it as a
   cross-lane edit; list every such edit in the report. CMake: append one block at the end of the owning list.
7. Implement first, then the tests named in the brief (they are the acceptance contract) plus what pins behaviour.
   Run pytest yourself on synthetic data: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   <files>`. C++ tests are written, not run; name the anchored gtest filters root will run.
8. Commit per task: `git add <your files>`, conventional message, trailer
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
9. Report: `.superpowers/sdd/platform-p9-20261003/task-<ID>-report.md`, committed with `git add -f`, in the
   `.agents/harness/TEMPLATES.md` "Lane report" shape: outcome, branch / SHA, files changed, evidence (each pytest
   command with exit code 0 and output tail), how root verifies (build targets, gtest filters, identity runs with
   argv), deviations, cross-lane edits, open risks, 0-3 ledger candidates.
10. Final reply to the PM: at most 15 lines (status DONE / DONE_WITH_CONCERNS / BLOCKED, commit SHAs per task, one test
    line, concerns). An adversarial reviewer reads your exact SHA before merge; fix rounds get a new review.

---

## Wave 1 (base `d7c1c520`; dispatched at plan R0-1; merged after the v8 report, slots in plan §3.2)

## Lane E1: wave driver hardening (task 0 = P0-FIX, the Phase 0 blockers)

**Pool / branch:** 17, `feat/p9-e1-20261003`. **Effort:** M (task 0: S, deliver first). **Serves:** infrastructure.
**Read:** `docs/plans/2026-10-02-p9-code-review-orchestration.md` (all), `docs/plans/2026-10-02-v8y-research-loop.md`,
main review F-5, F-7, F-9, NV-4 (nav review §4 "Completeness hole"), plan §1.2.
**Task 0 (P0-FIX; commit it alone, first, and report its SHA at once: root merges exactly that commit before Y-S):**
(a) `RUNNER_MAX_SECONDS = 600` in `scripts/research_tree.py`; `run_bounded_research.py:92-94` uses it; manifest load
(`wave_manifest.py:225-229`), spec load (`research_cycle.py:369-376`) and `wave_steps.py:181-183` refuse a phase cap
above it with a message naming the key; `test_wave_speed.py:80-86` stops asserting 720 against fakes. (b) budget
`admission_cycle_prefixes` (a list; the old string key stays valid) in `wave_manifest.py` and
`wave_stage_preflight.py:72`. (c) capacity completeness: a NAV phase whose argv has `--capacity-curve` is done only when
`summary.json`, `capacity_curve.csv` and `v7_extras.json` exist (`research_cycle.py:1176`); `wave_readers.py:79-81`
refuses None when the curve is expected. (d) `executable_sha256` kept in phase rows (`wave_stage_util.py:58-68`) and
`wave-result.json`; verify records parent vs cell NAV exe SHA and forces the ref pass when they differ
(`research_cycle.py:1015-1019`). (e) `O_EXCL` lock around verify-and-append (`backtest_integrity.py:1094-1118`; that
function only). (f) `scripts/tests/test_research_spec.py:60-91`: expected null pins derived from each spec's kind
(template / child / add-alpha copy / gm), not a file list. (g) a documented two-seed suite command. Root edits
`y-s.json` itself (DEC-1, DEC-2); you do not touch any registration file.
**Then (wave 1 proper):** K-P9-10 (`argv_sha256`, `attempt`, `executable_sha256`, `build_type` in every bounded
`receipt.json`; resume refuses a mismatch: OR-3); attempt sub-dirs `<output>/attempt-k/` with auto-advance when the
runner refused and wrote nothing (OR-4); bounded waiting launch admission (free >= declared peak + floor; no `cl.exe`,
`clang-cl`, `ninja`, `lld-link` process) (F-5 (a)); a host memory semaphore over declared caps so card || marginal,
ref || u and summ || bundle || reader run in parallel (OR §5); `lock` writes `exes_sha256`, verify compares parent and
cell (OR-2); receipt chain digests over content keys only (no `started_utc` / seconds; OR §3); queue history dated from
the manifest, not today; reader reuse keyed on the reader code SHA; `cycle_verdict.json` written per run, never
overwritten; complete timings (screen u / fit / card / marginal, register, readers, bundle, git) in `wave-result.json`
and `scoreboard --timings`; K-P9-11 keys (`source_sample_end`, `predicted_mechanism`, `data_class`) in `wave_queue.py`.
**Files in scope:** `scripts/run_bounded_research.py`, `scripts/research_tree.py`, `scripts/wave_*.py`,
`scripts/research_wave.py`, `scripts/cycle_resume.py`, `scripts/cycle_verdict.py`, `atx-engine/tools/stage_chain.py`,
their tests; task 0 only: `research_cycle.py` (the lines named), `backtest_integrity.py` (the lock),
`scripts/tests/test_research_spec.py`. **Forbidden:** any `scripts/specs/**` file, any C++.
**Tests (yours):** `test_runner_max_refuses_720` (manifest, spec, wave step), `test_budget_prefix_list_counts_v8ys`,
`test_capacity_missing_is_not_done`, `test_exe_sha_in_phase_rows`, `test_ledger_append_lock_excl`,
`test_spec_kind_null_pins`, `test_resume_refuses_argv_mismatch`, `test_attempt_subdir_after_floor_kill` (a planted
floor kill on fakes resumes to completion), `test_launch_waits_for_free_memory`, `test_receipt_digest_time_free`,
`test_timings_complete`. All of `scripts/tests` under `PYTHONHASHSEED=0` and `=1`.
**Root verifies:** `wave plan y-s.json` after the amendment; a tiny-world wave end to end; the next real wave's receipts.
**Out of scope:** splitting `research_cycle.py` (E2), any rule or NAV logic, the gm rule (C2).

## Lane A1: field registry and the freeze on Python builders

**Pool / branch:** 12, `feat/p9-a1-20261003`. **Effort:** M. **Serves:** infrastructure, significance (seal).
**Read:** `docs/plans/2026-10-02-p9-code-review-fields.md` (all), `docs/plans/2026-10-02-platform-core-migration.md`
§3-§4, audit §2 field rows, main review F-6, F-12.
**Deliver:** (1) K-P9-1 `atx-engine/tools/field_registry.json` + loader `field_registry.py`, generated once from today's
four registration mechanisms (inline dicts prep:194-278 / 411-440 / 480-504; `FIELD_MODULES` bind; the holdings wrap
prep:3186-3187; the engine shim engine.py:146-160), in today's manifest order, with declared `dtype`; a test that the
generated registry reproduces today's v15 field list and order on fixtures. (2) One entry `prepare_research_fields.py
--registry <json> --fields <list|all>` replacing the shim one-liners (ohlc:11-12); the four byte-identical shim
`register()` / `main()` bodies become thin deprecated wrappers over it; holdings onto `FIELD_MODULES` (reuse interface
holdings:1216-1245). (3) Freeze: `test_no_new_python_builder.py` fails on a `research_fields_*.py` /
`prepare_research_fields_*.py` outside a committed allowlist. (4) Reuse key and manifest record numpy, pyarrow,
duckdb and Python versions (FD-2); a test that every `PRODUCERS` closure's cross-module names are in `IMPORTS`. (5) Seal
(FD-5): remove the 2025 bind (`atx-engine/tools/conftest.py:15`) and regenerate the affected fixtures under the
repository window; `load_prior` (prep:2916-2931) and the field readers refuse `seal.exclusive_end` != the research
seal; rename `rows_available_on_or_after_2025_dropped` to `rows_sealed_dropped` (prep:948, 1689, 1754; the C++ key
`research_fields_cli.cpp:116` is A2's: declare the rename in the report). (6) Fix `engine.py:13`'s dead test reference.
**Files in scope:** `atx-engine/tools/prepare_research_fields*.py`, the registration blocks of `research_fields_*.py`
(no builder arithmetic), `code_fingerprint.py`, `atx-engine/tools/conftest.py`, new `field_registry.{py,json}`,
`atx-engine/tools/test_*` for these, `atx-engine/tests/fixtures/research_fields/` Python generator only.
**Forbidden:** any C++, `scripts/**`, builder arithmetic.
**Root verifies:** pytest `atx-engine/tools`; fields v15 rebuilt through the entry with `--reuse` into a new dir: 84
payload SHA-256 equal to v15's; manifest bytes differ only in the documented keys (one re-pin ruling).
**Out of scope:** porting any builder to C++ (A2, A3, A4); new fields.

## Lane A2: C++ field registry, builder kinds and the fields exe

**Pool / branch:** 13, `feat/p9-a2-20261003`. **Effort:** L. **Serves:** infrastructure.
**Read:** fields review §5 and §7 (FD-1), migration §1, §3, §4 slice 3, the headers under
`atx-engine/include/atx/engine/research/fields/` and `atx-engine/src/research/fields/` (all), A1's K-P9-1 (plan §2.3;
code against the schema, A1 merges first).
**Deliver:** `research/fields/registry.{hpp,cpp}`: `BuilderKind {id, parse, build}` (K-P9-2); `vol_126`, `si_shares`,
`si_dtc` registered as kinds (replace the name dispatch `research_fields_cli.cpp:28, 124-133`); `atx-research-fields
build --registry R --spec S --receipt OUT` reading K-P9-1; publish-last manifest and the reuse decision in the exe
(`manifest.{hpp,cpp}`, `reuse.{hpp,cpp}`; mig slice 3, not yet implemented); producer identity (K-P9-3) in every engine
entry, reuse of an engine entry keyed on it, identity claim on payload + coverage only (FD-1); the sealed-rows stats key
renamed `rows_sealed_dropped` (with A1); seal refusal on a prior manifest whose seal differs. Gtests:
`ResearchFieldsRegistry.*` (kind lookup, unknown kind refused, parse round-trip), `ResearchFieldsManifest.*`
(publish-last, producer block, reuse hit / miss on exe identity), the existing fixture identity unchanged.
**Files in scope:** `atx-engine/include/atx/engine/research/fields/**`, `atx-engine/src/research/fields/**`, the fields
block of `atx-engine/CMakeLists.txt` (:266-282), `atx-engine/tests/research_fields/**`,
`atx-engine/tools/prepare_research_fields_engine.py` (becomes a registry `kind: engine` caller).
**Forbidden:** other `atx-engine/tools/*.py` (A1), `atx-engine/tests/CMakeLists.txt` CTest lines (T1).
**Root verifies:** builds `atx-engine-research-fields`, `atx-engine-research-fields-tests`, `atx-research-fields`;
`--gtest_filter=ResearchFields*`; fixture identity (8 x 300); the three engine fields on the TRAIN role equal v15's
payload SHA-256.
**Out of scope:** parquet sources (A3); new builders.

## Lane B1: `factors` verb and `research/admission` in C++

**Pool / branch:** 14, `feat/p9-b1-20261003`. **Effort:** L. **Serves:** significance, infrastructure.
**Read:** composition review (all; CM-3, table rows 6-7, §4 "Sign rule"), main review F-3, audit:127-129,
`atx-impl/tools/fit_composition_weights.py:1130-1170` (factor record), `:1572-1583` (NW t), `:1613-1671` (screen_v4),
`:1641-1670` (greedy), `atx-impl/src/strategy_exposures_verb.{hpp,cpp}`, `atx-engine/include/atx/engine/eval/hac.hpp`.
**Deliver:** (1) `atx-equity-strategy-targets factors` (K-P9-4) in new `strategy_factors_verb.{hpp,cpp}`: the
neutralised gross-1 rank book q per candidate on the exposures basis, f = q . r(d+2), tau; one dispatch line in
`equity_strategy_targets.cpp`. (2) `atx-engine-research-admission`: `screen_v4` (no_prior; < 250 live days; tau > .70;
NW HAC t < -2.0, Bartlett lag 5, divisor n, no small-sample correction; first failure wins) on
`eval::hac::mean_inference` with a method value that reproduces the fitter's arithmetic; greedy redundancy (|rho| > .90
over >= 250 common days; (tier, roster index) order; strict >); the PM7-35 sign rule as one predicate parameterised by
the PM's ruling (the gate requires runner sign = prior, `wave_rules.py:50-51` keeps sign 0: write both, the PM picks
one before merge); the traded-horizon columns (IC and HAC t at 21-session overlap) as report-only output (F-3).
(3) A comparator pytest that imports the fitter's `screen_v4` / `factor_record` as library functions on synthetic data
and checks the C++ outputs' committed fixture bytes (no fitter edit). Gtests: `ResearchAdmission.ScreenV4ClosedForm`,
`.FirstFailureWins`, `.GreedyOrderTierThenRoster`, `.SignPredicate*`, `FactorsVerb.EqualsFixture`,
`FactorsVerb.FutureReturnDoesNotChangePast`.
**Files in scope:** new `atx-engine/{include/atx/engine,src}/research/admission/**` and its CMake block and tests, new
`atx-impl/src/strategy_factors_verb.{cpp,hpp}`, the dispatch line in `atx-impl/tools/equity_strategy_targets.cpp`, the
`atx-impl` CMake source line, new tests and fixtures. **Forbidden:** `fit_composition_weights.py` (D1 owns it in wave 1;
D2 wires it), `strategy_ic_admission.cpp` (D1).
**Root verifies:** builds `atx-equity-strategy-targets`, the admission library and tests; factor series of X-5's library
equal the fitter's records (bytes, else 1e-12 with the reason); X-5's and the Y-S screen's `admission.csv` reproduced
byte for byte by the C++ screen.
**Out of scope:** fit verbs (D2), changing the gate's horizon (a ruling).

## Lane C1: per-book NAV config, capacity in the main lockstep, Release-safe cost, complete receipts

**Pool / branch:** 15, `feat/p9-c1-20261003`. **Effort:** L. **Serves:** gross return, capacity, infrastructure.
**Read:** `docs/plans/2026-10-02-p9-code-review-nav.md` (all; NV-1..NV-4, §4, §5), `strategy_nav_replay.cpp` (layout
in NV §1), `strategy_nav_v7.{hpp,cpp}`, `strategy_vol_target.*`, `strategy_risk_target.*`,
`atx-engine/src/book/replay_cost.cpp`, `strategy_cost_v2.{hpp,cpp}`, the YCOMB review's two composition bugs
(`review-ycomb.md:9-10`).
**Deliver:** (1) the leverage rule (fixed L, `vol-target-v1`, `risk-target-v1`) as a per-book member of
`NavReplayConfig` with per-book state, replacing the thread-local hook (v7:103), so `--book-workers` and the
`--aim-leverage` grid key work under the scalers (today refused, nav:2299-2304, 3220-3221); the leverage part of
K-P9-7 (`nav --list-rules --json`). (2) Capacity books in the main lockstep (book cap 8 -> 16; identified by
`capacity_multiple(id)`, v7:503, 535) instead of the argv re-dispatch (v7:1121-1133). (3) `sqrt(x)` where the impact
exponent is .5 (`replay_cost.cpp:92`); `pow` stays for other exponents (`strategy_cost_v2.cpp:45,138,196,231`); a probe
test of `cost_fraction` bits on fixed inputs. (4) `summary.json` written last, binding `v7_extras.json` and the
capacity summary (v7:349-430, 1107, 1134); exe identity (git SHA, build type) and the argv SHA-256 in the recipe.
(5) adv-hold capacity reads each multiple's NAV, not the initial NAV (nav:800; update `strategy_live_test.cpp:2234`'s pin
only with the reason). (6) A truncation / look-ahead test on the scaler path (NV §4 GAP). Gtests: `NavBookRule.*`
(grid at L {1.0, 1.5, 2.0} in one pass == three single runs, byte for byte; vol-target under `--book-workers 4` ==
serial), `NavCapacityLockstep.*` (curve == the separate-dispatch curve), `ReplayCostSqrt.*`, `NavSummaryBinding.*`,
`VolTarget.TruncationInvariant`, `AdvHoldCapacityPerMultiple`.
**Files in scope:** `atx-impl/src/strategy_nav_replay.{cpp,hpp}`, `strategy_nav_v7.{cpp,hpp}`,
`strategy_vol_target.*`, `strategy_risk_target.*`, `strategy_cost_v2.{cpp,hpp}`, `atx-engine/src/book/replay_cost.cpp`,
their tests in `atx-impl/tests/` and the engine book group. **Forbidden:** `strategy_target_replay.cpp` beyond the
call sites the rule move needs (list them), `scripts/**`.
**Root verifies:** builds `atx-equity-strategy-targets`, `atx-impl-strategy-target-tests`, the engine book group;
X-5's NAV re-run under the new Debug build: every file byte-identical except the list you state before the run
(summary keys added; cost columns only if `sqrt` moves a bit); then a Release NAV build compared to Debug bit for bit.
NV-3 and DS-1 disagree on the 1-ULP cause; your probe test is the evidence; state in the report what root should expect.
**Out of scope:** `--calibrate-gross` (C2), NavSpec / registries / file split (C3), L > 2.

## Lane D1: composition rule table, ordered stages, one theme table

**Pool / branch:** 16, `feat/p9-d1-20261003`. **Effort:** M. **Serves:** Sharpe (new rules become cheap), infrastructure.
**Read:** composition review (all; CM-4, §3 touch points, table row 8), main review F-8, F-13, the DS review §3
"Memory", `strategy_ic_admission.cpp:413-840` and `:233-287`, `strategy_ic_runner.cpp:282-289`,
`strategy_ic_composition.{hpp,cpp}`, `strategy_ic_detail.hpp:146-176`, `fit_composition_weights.py:247-267, 2337-2473,
280-291`.
**Deliver:** `strategy_ic_rules.{hpp,cpp}`: a `CompositionRule` table `{id, block_key, parse, verify, apply_stage,
recipe_text, working_bytes}` replacing the 4-row `theme_standardise` table (`admission.cpp:500-526`) and key-presence
dispatch (`:814-835`); `IcComposition` takes an ordered stage list (replacing the 20-parameter `score_role` list);
`atx-equity-strategy-ic --list-rules --json` (K-P9-6); the theme list read from
`atx-impl/strategies/alphas/registry.json` and passed to the exe (replaces `strategy_ic_theme_resid.hpp:24-29` and the
theme column of `strategy_two_speed.hpp:27-32`; the fitter's theme tuple reads the registry); a Python rule plugin list
replacing the fitter's if / elif (dispatch only, no arithmetic change); one centred-tied-rank helper (3 C++ copies, CM
§2); the memory admission prints `required_bytes` from `--plan-only` even when over the cap (`:283-285`) and documents
the worst-candidate + theme-plane peak. Gtests: `CompositionRules.TableCoversEveryRule`,
`.RecipeTextPinned` (byte-equal to today's recipe text per rule), `.IncompatiblePairRefused`, `.ThemeTableFromRegistry`,
`IcAdmission.PlanPrintsRequiredBytesOverCap`; pytest for the plugin list and the registry theme read.
**Files in scope:** plan §2.2 row D1. **Forbidden:** `strategy_marginal_ic.*` (S1), the NAV files (C1).
**Root verifies:** builds `atx-equity-strategy-ic`, `atx-impl-strategy-ic-tests`; X-5's fit, u and w byte-identical
(recipe text pinned); the list-rules JSON pinned; pytest `atx-impl/tools`.
**Out of scope:** any new rule (AL-COMB), fit verbs (D2), weights v2 (D3).

## Lane S1: marginal verb speed and the Release IC exe

**Pool / branch:** 18, `feat/p9-s1-20261003`. **Effort:** M. **Serves:** infrastructure (wave wall time).
**Read:** composition review §1 "Marginal verb", §2 C++-internal duplicates, §4 "Marginal rows for book members", §5,
CM-1, CM-6; DS review §3 "Cache-key completeness" and "Debug vs Release", §7 item 1; v8y loop §4 design notes;
`strategy_marginal_ic.cpp`, `marginal_rank_ic.{hpp,cpp}`, `orthogonalize.cpp`, `strategy_ic_signal_cache.cpp:41-151`,
`strategy_ic_result_cache.cpp:29-48, 114-124`.
**Deliver:** `--candidates ID,...` (residualise only the listed ids; pairwise rho only for pairs with a listed id);
rows compacted to the role's decision members; a pair cache keyed by (role SHA, payload_a, payload_b, min_names,
method version), reusable across waves on a role; no re-hash of payloads the u pass verified (accept verified
digests); theme-regressor cap 10 -> 33 (`marginal_rank_ic.hpp:45`); DetPool date bands with lane-owned writes; timers
(hash / kernel / pairwise) in the output; the engine `research_return_guard` reused instead of `:359-407`; the copied
helpers `:52-92` replaced by the shared ones; the member-row bias fix (composite excluding the member) only behind
`--exclude-self`; a token for build type, NDEBUG, CRT flavour and xsimd version in `vm_identity` / `ic_identity`
(`ic_identity` from `ic_screen.cpp`'s TU); the Release equity preset target for `atx-equity-strategy-ic`
(`CMakePresets.json`). Gtests: `MarginalIc.CandidatesSubsetEqualsFullRows`, `.CompactedRowsEqual`,
`.PairCacheHitEqualsCompute`, `.ThirtyThreeThemes`, `.BandsByteIdenticalAt1And4`, `IcIdentity.BuildTokenSeparatesCaches`.
**Files in scope:** plan §2.2 row S1. **Forbidden:** `research_cycle.py` (add `--candidates` to `MARGINAL_SPEC_FLAGS`
is a one-line cross-lane edit you list; E2 owns the file in wave 2).
**Root verifies:** builds Debug and Release `atx-equity-strategy-ic` and `atx-impl-strategy-ic-tests`; the alpha
oracle and conformance suites under Release (NDEBUG compiles out `ATX_ASSERT`); flag-absent marginal rows
byte-identical; Release u / w byte-identical to Debug on X-5; marginal wall per pass logged (target <= 30 s).
**Out of scope:** VM kernels (S2), the gate (B1).

## Lane T1: tiny-world canary, CTest registration, statistics tie fixture, guard tests

**Pool / branch:** 19, `feat/p9-t1-20261003`. **Effort:** M. **Serves:** significance, infrastructure.
**Read:** main review F-1, F-14, F-10, P9-R10; fields review §1 (fields tests not in CTest, `atx-engine/tests/
CMakeLists.txt:364-367`); DS review §6; audit §4 (class-C list and deletion rule); `scripts/tests/fixtures/
tiny_world.py`, `scripts/tests/test_cycle_e2e.py`, `scripts/research-build.ps1`, `atx-impl/tools/backtest_integrity.py:
160-486`, `nav_summ.py:420-463`, `dsr_total.py`, engine `eval/{deflated_sharpe,min_trl,pbo,trial_clusters}.hpp`.
**Deliver:** (1) the canary: `test_cycle_e2e.py` runs u / fit / w / NAV on the real exes when `ATX_EQUITY_BIN` is set,
pins SHA-256 goldens (root records them), and checks the planted members' mean IC and HAC t within one SE of the planted
value; `research-build.ps1 -Canary` runs it after a build. (2) CTest registration with labels `atx_research` /
`atx_equity_strategy` for `atx-engine-research-fields-tests` and every strategy / research test exe. (3) The tie
fixture `atx-engine/tests/fixtures/eval_tie/`: committed daily series (synthetic), a generator that runs today's Python
(PSR, DSR house definition, MinTRL, PBO, ONC, paired CBB dSR with block 21 / seed 20260929 / 4,999, Memmel SE) and
stores its values, and a gtest that reproduces each from the engine headers (tolerance 0 where reduction order allows,
else 1e-12, the reason stated per value); a value the engine cannot reproduce is reported, not loosened. (4) Guards:
`test_no_versioned_scripts.py` (allowlist frozen at base), `test_no_python_mirror.py` (allowlist rows with the lane that
retires each). (5) The class-C deletion as one separate commit (audit §4 list and their tests), merged only after root's
`generate_from_spec.py --spec specs/library-v71.json --check` exits 0.
**Files in scope:** plan §2.2 row T1. **Forbidden:** the Python statistics themselves (B2 deletes them in wave 2).
**Root verifies:** canary goldens on Debug, then Release; `ctest -N -L atx_research` count in the log; the tie gtest.
**Out of scope:** the eval verb (B2), the CMake split of `atx-impl-core` (C3).

---

## Wave 2 (base = the P9 integration head after the wave-1 merges and the P9-B0 re-base; root names the SHA)

## Lane A3: one shared vendor panel in C++, price and ohlc builder kinds

**Pool / branch:** 12, `feat/p9-a3-20261003`. **Effort:** L. **Serves:** infrastructure (fields build time).
**Read:** fields review §3 (look-ahead rules per source, seal readers that decode sealed rows), §4 (5 TickerHistory3
readers), §6 (7 SHA passes, ~10 scans), FD-3, FD-5; migration §4 slice 4; A2's registry and K-P9-2;
`research_fields_price.py`, `research_fields_ohlc.py`, `prepare_research_fields.py:1019-1135` (th group,
factor_breaks), `repair_role_factor_breaks.py`.
**Deliver:** `research/fields/sources/vendor_panel.{hpp,cpp}` (vcpkg arrow / parquet): hash the file once, one scan over
the union of requested columns, the `tradingDate < seal` filter pushed down (sealed rows never decoded), the
observation contract of the price module applied once; factor-break-v1 in C++ once per run (one implementation for the
two Python copies, audit §3 row 2); builder kinds for the price-module and ohlc fields on it; registry rows `kind:
engine` for each moved field (added to A1's JSON). Gtests: `VendorPanel.HashOnce`, `.SealPushDown` (a planted 2024 row
is never decoded), `FactorBreak.ClosedForm`, one planted-leak probe with teeth per moved builder, fixture byte identity
vs the Python builders.
**Files in scope:** `atx-engine/{include/atx/engine,src}/research/fields/sources/**`, new builder files under
`research/fields/`, their tests and fixtures, rows in `field_registry.json`. **Forbidden:** the Python builders (they
are deleted one slice after root's identity).
**Root verifies:** fields targets build; per moved field the TRAIN payload SHA-256 equals v15's; fields build wall
before / after.
**Out of scope:** SEC / holdings (A4).

## Lane B2: one engine `eval` verb and the ledger library

**Pool / branch:** 14, `feat/p9-b2-20261003`. **Effort:** L. **Serves:** significance.
**Read:** main review F-1; orchestration review §3 "Ledger", "DSR/PBO duplicated", OR-5, OR-6; audit §3 rows 4-5;
migration §4 slice 8; T1's tie fixture; lit §5.1-§5.2 ([70], [73], [74]); `nav_summ.py`, `backtest_integrity.py`,
`dsr_total.py`, `scripts/research_ledger.py`, `atx-impl/src/trial_ledger.{hpp,cpp}`, engine `eval/*.hpp` named above.
**Deliver:** `atx-research-eval` (K-P9-5) over the engine headers plus the paired CBB dSR and Memmel SE ported from
`nav_summ.py`; DSR `house_v1` (N = construction count, V = window cell variance; `backtest_integrity.py:1254-1277`)
beside the engine's named methods; the winner's-curse-adjusted cumulative gain (Andrews-Kitagawa-McCloskey [73]),
printed only; `atx-engine-research-ledger` (trial_counts, N_tot, era pooling; `trial_ledger.cpp` moved down, atx-impl
keeps a thin include); the record stage writes the ledger head into the sprint dir (cross-lane edit in
`wave_stage_record.py`, listed). `nav_summ.py --engine-stats` calls the verb; the Python statistics stay until root's
identity, then a separate prepared commit deletes them. Gtests: T1's tie fixture through the verb, `EvalVerb.JsonSchema`,
`ResearchLedger.TrialCountsEqualPython`, `Conditional.ClosedFormNormalCase`.
**Files in scope:** plan §2.2 wave-2 row B2. **Forbidden:** acceptance rules (`wave_rules.py`).
**Root verifies:** builds the eval and ledger targets; `nav_summ --protocol v8` on X-5 and on Y-F0 prints identical values
with and without `--engine-stats`; the ledger chain verifies.
**Out of scope:** any change to which statistic decides (PM7-34 stays).

## Lane C2: `--calibrate-gross` in C++

**Pool / branch:** 15, `feat/p9-c2-20261003`. **Effort:** M. **Serves:** infrastructure, gross return.
**Read:** nav review §5 (G(L) not linear; NAV scale the only exact dimension), NV-5; v8y loop §4 design note;
`scripts/wave_rules.py:110-119`, `wave_stage_cell.py:92-130` (match stage), C1's merged per-book config.
**Deliver:** `nav --calibrate-gross TARGET --calibrate-tolerance .005`: pass 1 at L, S2 book only, cached desired
targets, no capacity books; if outside tolerance PM6-6's single correction L' = L x G_P / G rounded to 4 decimals and
pass 2 at L' in the same process (fields, liquidity cache, combined signal loaded once); both L and both gross values in
the recipe; refusal when L' leaves the leverage range. The match stage calls it (cross-lane edit in
`wave_stage_cell.py`, listed); `wave_rules.py:110-119` deleted after root's identity (G-P5). Gtests:
`CalibrateGross.WithinToleranceOnePass`, `.OneCorrectionMatchesTwoProcess`, `.RefusesOutsideRange`.
**Files in scope:** the C1 NAV files, `atx-impl/src/config.{cpp,hpp}`, `scripts/wave_rules.py:110-119`, tests.
**Root verifies:** X-5's gm pair: the matched NAV byte-identical to the two-process result; wall before / after.
**Out of scope:** NavSpec (C3).

## Lane D2: C++ fit verbs; retire the composition mirrors

**Pool / branch:** 16, `feat/p9-d2-20261003`. **Effort:** L. **Serves:** infrastructure, significance.
**Read:** composition review §2 (duplicates table), §4 "Float identity", CM-3, CM-5; migration §1 `composition/`;
B1's K-P9-4 and admission library; D1's rule table; `composition_*.py`, `fit_composition_weights.py`.
**Deliver:** `atx-engine-research-composition` (the `strategy_ic_{shrink,theme_resid,theme_erc,theme_tsmom,two_speed}`
rules relocated, atx-impl files become thin includes) and `atx-research-composition fit` computing ic-shrink, the ERC
covariance, theme-resid and the tsmom sleeves from the factor series; ew-theme-std tier weights and member cap in C++
(CM row 5b: no check today; row 5: summation order differs, pin C++ order and state it); the fitter calls `fit` and B1's
admission behind `--engine-fit`; output bytes no longer embed the fitter's SHA (CM-5: producer fingerprint of the
writing functions); the `composition_*.py` deletion as a separate prepared commit. Gtests per rule: closed form (ERC on a
2 x 2 covariance, F-15), equality with the old Python fixture values.
**Files in scope:** plan §2.2 wave-2 row D2. **Forbidden:** walk-forward (D3).
**Root verifies:** X-5's fit byte-identical with `--engine-fit` (the PM7-30 substitution list shrinks to empty after
CM-5); ic-tests; pytest `atx-impl/tools`.
**Out of scope:** new rules.

## Lane E2: split `research_cycle.py`; manifest kinds; capabilities from the exes

**Pool / branch:** 17, `feat/p9-e2-20261003`. **Effort:** L. **Serves:** infrastructure.
**Read:** orchestration review §1 (ranges), §4 (copy-paste list, implicit path coupling), §6 table; K-P9-1, -6, -7, -9,
-10; DS review §4 ("flags bypassing the spec").
**Deliver:** move-only split of `research_cycle.py` into `scripts/cycle/{spec,resolver,phases,gate,compare,lock,cli}.py`
(the CLI and argv byte-identical); `scripts/research_common.py` holding the duplicated helpers (sha256 x6, `fmt_argv`,
execute, ledger readers x3, run-dir allocation x2, dirty check x2); run-dir naming defined once and imported by the wave
(OR §4: a rename silently shrinks the seal scan); `atx.research-wave/v2` (K-P9-9) with `rules:` validated against the
exes' list-rules JSON; kinds `field` (builds the fields dir from registry rows), `role` (chains role -> fields -> cycle;
role builder stays Python), `horizon` (renames u / fit / w outputs), `leverage`, `walk-forward` (schema only; D3 wires
it); capabilities from `--list-rules --json` instead of `--help` probing (`research_cycle.py:676-687, 950-952`);
`scripts/specs/p9/kinds/*.json` fixtures for the 10 kinds; `--candidates` in `MARGINAL_SPEC_FLAGS` (if S1 did not).
**Files in scope:** plan §2.2 wave-2 row E2 and the new `scripts/cycle/`, `scripts/specs/p9/kinds/`.
**Root verifies:** every v8 spec and wave manifest plans byte-identical argv before / after (`plan` diff empty); the kinds
fixtures pass on tiny-world; `scripts/tests` under two seeds.
**Out of scope:** new acceptance rules.

## Lane S2: VM kernels in parallel, one `OpSig` table, IC label and coverage options

**Pool / branch:** 18, `feat/p9-s2-20261003`. **Effort:** L. **Serves:** infrastructure, significance.
**Read:** `docs/plans/2026-10-02-p9-code-review-dsl-ic.md` (all; DS-2..DS-5, §3 HAC and labels, §6); `vm.hpp:720-730,
1348-1372, 1399-1443, 1450-1495, 1498-1535`; `registry.hpp:74-359`; `typecheck.hpp:88-157`; `streaming_engine.hpp:
202-231`; `oracle.{hpp,cpp}`; `ic_screen.cpp:63, 203, 242-260, 283-358`; `ic_screen_config.hpp`.
**Deliver:** band-split of the serial kernels by the declared `chunk_axis` with per-worker scratch (as `ts_col_thr_`);
columns extracted once per instruction in `eval_lit_ts`; one constexpr `OpSig` table with family, axis, needs_group,
stateful (static_assert size == last opcode + 1), every predicate derived from it, `lookahead_safe` derived;
`IcScreenConfig::min_coverage` (default .8) keyed in the IC-result scope; `--label-terminal imputed-v1` (label-only
price column with imputed terminal returns; report the missing-exit share and IC under both rules); `--eval-mode
audit-exact` with the cache under its own identity token; the IC runner's HAC rule named and selectable beside
`eval/hac.hpp`'s (default unchanged). Gtests: worker parity 1 / 4 / 16 for every banded kernel, a per-opcode
truncation-invariance and slot-reuse sweep over all opcodes including `formulaic_ops()`, `OpSig.PredicatesAgree`
(streaming and VM), `IcScreen.MinCoverageConfig`, `IcLabels.TerminalImputedClosedForm`.
**Files in scope:** plan §2.2 wave-2 row S2. **Forbidden:** the marginal verb (S1's), composition (D lanes).
**Root verifies:** alpha / factory groups and the IC exe build; golden `0x889874a3b9b29c55` at 1 and 4 workers;
`AlphaVmSlotReuse.*`; X-5 u / w byte-identical with every flag absent; cold u wall before / after.
**Out of scope:** adopting the label or AuditExact default (P9-L cell and a P9-B0 ruling).

## Lane AL-COMB: theme-cov-hedge-v1 and mom-volman-v1 (blind registrations)

**Pool / branch:** 19, `feat/p9-alcomb-20261003`. **Effort:** M. **Serves:** Sharpe.
**Read:** lit §1.1 [21], §1.6 [16]-[20], §6 rows 1 and 7, §7 (DMRS caveat, restatement risk), §8 Q5; plan DEC-15,
DEC-16; D1's rule table and K-P9-6; `strategy_ic_theme_tsmom.{hpp,cpp}` (schedule mechanism); `review-x5-theme-erc.md`.
**Deliver:** (1) `theme-cov-hedge-v1`: kernel `atx-engine/include/atx/engine/combine/char_cov_hedge.hpp` (per theme:
beta_perp of each name on its own theme sleeve return over the trailing 252 decisions ending d-2, orthogonalised to the
score; h by trailing regression re-estimated every 21 decisions; w = rank(score) - h . rank(beta_perp)), estimated
causally at apply time; runner rule `atx-impl/src/strategy_ic_theme_hedge.{hpp,cpp}` as one row in D1's table.
(2) `mom-volman-v1`: the price_momentum sleeve mass scaled by sigma_target / sigma_hat (126 sessions), sigma_target the
running mean of the sleeve's own estimates (the real-time form of [17]), as a schedule rule beside theme-tsmom.
(3) A zero-trial diagnostic spec for lit §8 Q5 (the book's variance share on risk-store style columns that carry no
theme) for root to run before P9-H is registered. (4) Templates `scripts/specs/p9/templates/{theme-cov-hedge,
mom-volman}.json` with `rules:` blocks; rule registration files with K-P9-11 keys, the constants and why each is fixed.
Gtests: `CharCovHedge.ClosedForm2Names`, `.FutureReturnDoesNotChangePast`, `ThemeHedgeRule.FlagAbsentIdentity`,
`MomVolman.StepVarianceClosedForm`.
**Blind:** no constant chosen after any read; one variant each; no grid. **Root verifies:** ic-tests; X-5 w byte-identical
with both rules absent.
**Out of scope:** running anything; info-clock (AL-CLOCK).

## Lane AL-SIG: P9 signal set (blind registrations; new fields as C++ builder kinds)

**Pool / branch:** 20, `feat/p9-alsig-20261003`. **Effort:** L. **Serves:** Sharpe, capacity.
**Read:** lit §2.5, §3.0-§3.1 (F1, F2, F3, F8 rows), §5.2 (b), §6 rows 3, 4, 6, 8; plan DEC-5, DEC-14; K-P9-1, -2, -11;
`research_fields_mgr13f.py` and `research_fields_connected.py` (13F inputs, read only), the house index docs named in the
literature review header (documents only; never `atx-db/` working files or data).
**Deliver:** C++ builder kinds and registry rows for `gia_13f` (skill-weighted 13F holdings [61]: manager skill from
the manager's own past holdings returns; quarter-end plus filing-lag clock; 13F from 2013q2) and `russell_recon`
(predicted R2000 additions / R1000 deletions around the June reconstitution [56], from the house Russell proxy with its
rank-date clock, 2018-06+); at most 10 frozen DSL strings in total, each with theme, tier, the prior sign inside the
string, horizon class and half-life (PM8-5), citation, `source_sample_end`, `data_class`, `predicted_mechanism`;
`lazy_prices` and `hf_crowd` only as `blocked_on_data` registrations. Every builder has a planted-leak probe with teeth.
**Blind:** as AL-COMB; the bar for a string is lit §3.0's (net SR_c >= .4 and rho <= .1 to the book), argued from the
mechanism, never measured. **Root verifies:** fields targets build; K1 `--plan-only` of every string; fixture tests.
**Out of scope:** screening (root's P9-S); Python builders (frozen).

---

## Wave 3 (base = the P9 integration head after the wave-2 merges and re-base)

## Lane C3: typed NavSpec, cost-law and trade-rate registries, the NAV file split

**Pool / branch:** 15, `feat/p9-c3-20261003`. **Effort:** L. **Serves:** infrastructure, gross return.
**Read:** nav review §1 (layout), §2 (67 flags in two parsers; compiled constants), §3 (cost of a rule), NV-6; main review
F-8, F-10, F-11; C1 / C2 as merged.
**Deliver:** task 1, move-only: split `strategy_nav_replay.cpp` at the reviewed seams (validation, cost / liquidity,
MARK, EXECUTE, DECIDE, writers, loaders, summarize, publish, CLI) into `atx-impl/src/book/*.cpp`, byte-identical
outputs; commit alone. Task 2: NavSpec JSON (`nav --spec S.json`), the flag parser kept as a thin adapter producing the
same NavSpec; cost-law registry (FlatBps, SqrtImpact, the v2 laws) and trade-rate registry (aim-partial-v5,
per-name-v1, two-speed) completing K-P9-7; compiled constants (initial NAV, liquidity window, min pairs, capacity
multiples) become NavSpec fields with today's defaults; the leverage range a NavSpec field (default [1, 2]; wider only
by OD-P9-3). Task 3: split `atx-impl-core` into `atx-impl-strategy` and `atx-impl-pipeline` (F-10). Gtests:
`NavSpec.FlagsAndSpecSameConfig`, `CostLawRegistry.*`, `TradeRateRegistry.*`, the existing NAV suite unchanged.
**Files in scope:** the NAV files, new `atx-impl/src/book/**`, `atx-impl/CMakeLists.txt`, tests.
**Root verifies:** X-5 and Y-F0 NAV via `--spec` byte-identical to the argv form; full target-tests suite; build time of
the research exes before / after the core split.
**Out of scope:** new rules (AL-CLOCK adds the first).

## Lane D3: walk-forward refit as a spec kind

**Pool / branch:** 16, `feat/p9-d3-20261003`. **Effort:** L. **Serves:** significance, Sharpe (honest estimates).
**Read:** main review F-2, P9-R7; composition review CM-2, §1 "Walk-forward", §4 theme-tsmom timing; review-x5 (via
st7:14-15); D2 as merged; `combine/walk_forward_combiner.hpp`; `strategy_ic_composition.cpp:409-420` (theme schedule).
**Deliver:** K-P9-8 weights v2 (rows keyed by decision date, each fitted on decisions <= d-2); the runner applies
per-date weights (generalising the theme schedule); `atx-research-composition fit --walk-forward expanding --lag 2
--step 21`; theme-tsmom sleeves computed in C++ at apply time from the C++ factor series (CM-2's long-term fix: the
rule stays defined out of sample); the `walk-forward` manifest kind wired (E2's schema). Gtests:
`WeightsV2.SingleRowEqualsV1Bytes`, `WalkForward.ExpandingMeanClosedForm`, `.NoRowUsesFutureDecisions`,
`ThemeTsmom.ApplyTimeSleevesEqualRecorded` (on TRAIN fixtures).
**Root verifies:** v1 weights byte-identical when the kind is absent; tiny-world walk-forward fixture; ic-tests.
**Out of scope:** running P9-W (root's cell).

## Lane AL-CLOCK: info-clock-rate-v1 (blind registration; dispatched after C3 merges and DEC-16 is ruled)

**Pool / branch:** 19, `feat/p9-alclock-20261003`. **Effort:** M. **Serves:** gross return, Sharpe.
**Read:** lit §2.3 [28][29], §1.5, §7 "Restatement risk"; plan DEC-16; v8y §6 Y-5 row and §11 (two-speed table);
`book/two_speed.hpp`, `strategy_target_replay.cpp:345-399`, C3's trade-rate registry.
**Deliver:** `info-clock-rate-v1` as a trade-rate registry entry: theta_f = .12945 (Y-5's fast rate, half-life 5) for a
name while `ea_days_since` <= 21 (one month, [29]), else the parent's .05; a per-name rate vector in aim-partial-v5
generalising `two_speed.hpp`; refusal on parents the rule cannot compose with (listed in the registry row); the
restatement argument for the PM (Y-5 buckets themes, this rule buckets name-days by an event clock; R-3 is a per-signal
aim decay); template `scripts/specs/p9/templates/info-clock-rate.json`; registration file with K-P9-11 keys. Gtests:
`InfoClockRate.StepChangeClosedForm` (participation, not total traded quantity, changes), `.FlagAbsentIdentity`,
`.EventClockIsCausal`.
**Blind:** constants are Y-5's and [29]'s; no new constant. **Root verifies:** target-tests; X-5 NAV byte-identical with
the rule absent. **Out of scope:** running it.

## Lane AL-DATA: gated data families (registrations only for data that has landed)

**Pool / branch:** 20, `feat/p9-aldata-20261003`. **Effort:** M. **Serves:** Sharpe, significance (realism).
**Read:** lit §3.1 F1, F2, F4, F6, F10, §6 rows 3-5 and 9, §0 item 5; plan §6 OD-P9-4..6; the atx-db documents root names
at dispatch (documents only; never the `atx-db/` working tree).
**Deliver:** for each family whose data root reports landed: the C++ builder kind on A3's sources (text-change similarity
for Lazy Prices [39]; Form ADV private-fund flag joined to 13F filer CIK for crowding [46]; SG&A perpetual-inventory
intangible book for F10 [47], entered as a `replace` of `bm`; strike-level implied borrow [50][51] with the name-level
fee path into `book/borrow_schedule.hpp`), a planted-leak probe, a registry row and the registration file
(`source_sample_end` from the paper). For each family not landed: a one-page design in the report (fields, clock, seal,
builder kind, string), no code.
**Blind:** as AL-SIG. **Root verifies:** fields targets build; K1 plan of each string. **Out of scope:** `atx-db/`.

## Lane PRE: the P9 pre-registration

**Pool / branch:** 13, `feat/p9-pre-20261003`. **Effort:** M. **Serves:** significance.
**Read:** `v8y-prereg.md` (the model: §3-§8, §12-§14, Appendix B), plan §5 (all), lit §5 (all), DEC-16..DEC-18, the
AL-COMB / AL-SIG / AL-CLOCK / AL-DATA reports and registration files, B2's verb (`house_v1`, conditional print).
**Deliver:** `.superpowers/sdd/platform-p9-20261003/p9-prereg.md`: baseline and named books (P9-F0, P9-F); windows and
seal (unchanged); trial counting from v8's recorded end state; the budget table (plan §5.2) and order; per cell the
acceptance (PM7-34 unless stated), printed-only items, the mechanism prediction, the undefined / void rules; the
admission list (<= 10 strings, roster order, DSL SHA-256 and argv SHA-256 recomputed); contamination flags
(`source_sample_end`) for every member of the book and of P9; the OD-3 runbook (plan §5.3) with predictions; the
winner's-curse print; E[max] arithmetic; open choices as recommendation -- why -- cost if wrong. Every SHA-256
recomputed; inconsistencies across lane files listed.
**Blind:** no 2020-2023 statistic beyond the public lines of `progress.md` and the status files.
**Out of scope:** pinning (the PM rules; root pins).

## Lane A4 (stretch): SEC and holdings sources to C++

**Pool / branch:** 12, `feat/p9-a4-20261003`. **Effort:** L. **Serves:** infrastructure.
**Read:** migration §4 slice 5; audit §2 rows `research_fields_sec.py`, `research_fields_holdings.py`,
`research_fields_v9.py`; fields review §3 (clock spellings; probes without teeth in 8 groups).
**Deliver:** Calendar / Latest / Windowed clock primitives and the 13F / FTD / RegSHO stage readers in C++ on A3's source
interface; builder kinds for the moved fields with registry rows; a planted-leak probe with teeth per builder; fixture
byte identity vs the Python builders. **Root verifies:** TRAIN payload SHA-256 per moved field equals the current
manifest's. **Out of scope:** fundamentals events (slice 10, P10). If pools are short, this lane moves to P10 first.

---

## Adversarial review (one per lane head, read-only; paste with the lane id and SHA)

**You review lane `<id>` at exact SHA `<sha>` (base `<frozen-sha>`).** Read the lane's brief (above), its report, plan
§0.6 and §2.2, `.agents/cpp/agent.md` §10 and `.agents/harness/TEMPLATES.md` "Review". Read the diff with
`git -C C:/atx-wt/pool-2 diff <frozen-sha> <sha>` and files with `git show <sha>:<path>`; edit nothing, build nothing,
run no real data, open no 2020-2023 output and nothing dated 2024-01-01 or later. Re-run at least one of the lane's
pytest commands inside the lane's pool with `PYTHONDONTWRITEBYTECODE=1` and `-p no:cacheprovider` (no file written) and
paste its exit code and output tail as your evidence.
Look for: flag-absent identity broken (a default changed, an output key added unconditionally, an iteration order
changed); look-ahead (a window ending at d instead of d-1 / d-2; a label or return read before it matures); seal (any
path or row at or after 2024-01-01 decoded); a Python copy of the C++ rule kept or added; files outside the brief's
scope; `/W4 /WX` hazards (unused variables, sign conversion, shadowing, missing includes, 100-column limit); lifetime,
bounds and error paths; determinism (unordered iteration, thread-order writes); tests that are tautologies (YCOMB's
two-speed closed form at fixed L, `review-ycomb.md:24`) or that pin the implementation instead of the contract;
registry rows that disagree with the code; contract drift from K-P9-n.
Write `.superpowers/sdd/platform-p9-20261003/task-<id>-review.md` in the TEMPLATES "Review" shape (verdict APPROVE or
BLOCK; findings `path:line | severity | problem | required fix`) and return it to the PM in at most 12 lines. APPROVE
with a blocker or major open is invalid.
