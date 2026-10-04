# Lane COV review

## Verdict
**CHANGES_REQUIRED** (2 Required, 9 Suggested). Spec verdict: **CHANGES_REQUIRED** (R1). Quality verdict:
**CHANGES_REQUIRED** (R2). Everything else in the lane is sound: COV-4 / G-P11 trace verified, flag-absent identity
holds by code reading, container integrity is right on every accidental-corruption path, cross-lane edits are
minimal.

## Reviewed SHA
`18049a77` on `feat/p9-cov-20261003` (pool-24), base `ad406718`. Diff `ad406718..18049a77` read in full (code: 16
files, +5.7k lines; plus the report, handoff and `cov-evidence/`).

## Evidence
- Re-run (pool-24, explicit paths, `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`):
  `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_no_python_mirror.py scripts/tests/test_no_versioned_scripts.py`
  -> exit 0, tail: `G-P5: 37 mirrored rule function(s) in 14 row(s) left for P9 lanes` / `11 passed in 3.44s`.
- Lane gtest logs checked against receipts: `cov-gt-cov-all.log` 26/26 Cov* and `cov-gt-regress.log` 104/104 for the
  unchanged filter, both on `atx-impl-strategy-target-tests` from `p9-cov-e` (Source `ba2e2de6`, DirtyEntries 0);
  `ba2e2de6..18049a77` changes no file under `atx-engine/src`, `atx-engine/include`, `atx-impl` (only a bench cast and
  docs). Engine `cov-gt-engine-risk.log` 428 passed / 1 skipped (nightly) from `p9-cov-d` (`98e0f31b`; later commits
  touch no engine source).
- No gtest re-run and no build: no doubt below needs one (R1 needs the lane's per-rank evidence, R2 is by reading).
- Manifest / receipt reads for (b): listed under (b); no payload, statistic or post-2023 file opened.

## Mandatory answers

### (a) Ruling COV-5: the two restated test claims
**"< 2.0" was the lane's own threshold, but an absolute bias bound IS a requirement.** The brief names the test
`CovBias.EigenFamilyInBandOnAPlantedModel` (brief-COV.md:150) and cov-design §6 (cov-design.md:561) states the lane's
evidence as "planted-model bias in band". "In band" is the USE4 band the harness already reports per series
(1 +- sqrt(2/T) and the kurtosis-widened band; cov-design.md:218, `family_summary`). The lane's first version (pooled
b < 2.0) was a loose stand-in for that. It failed at 2.056. The current test (strategy_cov_model_test.cpp:483-499)
asserts b in (0.5, 3), b_v11 > 1 and b < b_v11, which is no band at all, under a name that promises one. → **R1.**

What explains 2.056: probably the fixture/config, not the kernel. The lane has not shown it, though, and its stated
reason is only partly right:
- **Not a fixture artifact:** the cap-constrained industry block (strategy_risk_model.hpp:19, sum_k cap_k f_k = 0). The
  real atx-risk-v1.1 has the same constraint, so its near-null eigendirection also shows up on TRAIN. In the fixture
  the industry cap shares stay constant inside each 25-session churn span (`cap = caps[i] x (1 + .001 (t % 7))`,
  strategy_cov_fixture.hpp:130). That makes the direction exactly null inside a span: it is floored at 1e-10 tr/m
  (strategy_risk_model.cpp:982), then realised off-null at each churn step. A pooled SD over all eigen ranks is
  dominated by that one rank. So the pooled statistic is a poor test statistic here, and D-COV will hit the same issue.
- **A design-level under-correction, made worse by `small_config`:** S2b simulates T = Kish count of the correlation
  HL-504 weights (strategy_risk_model.cpp:844, fixed by the brief). F's variances come from a much shorter EWMA:
  HL 21 in `small_config` (Kish about 61 against a simulated T of up to about 150), and HL 84 in the real recipe (Kish
  about 242 against about 1,100-1,450). The kernel therefore simulates a less noisy estimator than the one it corrects,
  so b stays above 1 by construction.
- **Not the kernel:** `eigen_adjust_v2` (pre-existing engine code) is used correctly: a = 1 overridden, 64
  simulations, per-session seed, refusals counted. On its own block it inflates small eigenfactors and leaves large ones
  near 1 (`SmallEigenfactorsInflatedLargeNearOne`). It lowers the true risk of the GMV portfolio when T matches
  (`LowersTrueRisk...`). On the model path it lowers b.

**The relative claim has weak teeth.** b < b_v11 catches a no-op S2b (the bytes would be equal, so not <) and gross
over-inflation (b > 0.5). It passes for **any** inflation of small eigenvalues, including a uniform scale-up or the
wrong n_eff. It cannot distinguish a correct adjustment from a merely positive one.

**Minvar test run "with structural forecasts off" (strategy_cov_model_test.cpp:516-540): accepted, and it is not a
weakening.** The brief/design put no requirement on the structural setting. `structural_factor_forecast` is a
pre-existing knob that the existing suite already uses (strategy_risk_model_test.cpp:492, :692). With the knob on,
every exposed factor of the planted panel is forecast, so the "complete forecasts only" rule would hold vacuously
(left-out count 0). Turning it off is what makes `complete_violations == 0` mean something; `excluded_incomplete > 0`
(:528) proves the rule was exercised. The residual gap is S7.

### (b) Ruling COV-6 / gate G-P11
**Model as-of: verified, no look-ahead in COV's code path.**
- Producer: block t is emitted after session t's pass (`update_prices` / `regress` / `update_vra` / `exposures` /
  forecasts all read rows <= t). The verb input layer adds no whole-axis statistic: `derive` and `panel_of` are
  cell-wise copies/ratios (strategy_risk_verb.cpp:211-272, :942-951). `close` is only used as same-line ratios
  (strategy_risk_model.cpp:629-633, :897-898). S2b depends only on (F_t, Kish_t from a causal recursion, the session's
  own seed).
- The truncation tests are real and would fail on a leak. `CovTruncation.*` runs `run_risk_model` on panels truncated
  after c (40 / 90 / 130), perturbed after c, or widened with future instruments, under **both** recipes. It compares
  the **entire block bytes** plus stamp, flags and session for every t <= c. The block carries every
  consumer-visible output (F, D, X, slot, the forecast flag) and every diagnostic (factor returns, lambda, bias,
  gammas). The perturbation has teeth: block c+1 must differ. `CovDiff.ThroughSessionReportsTheFirstDifferingBlock`
  repeats the perturbation through the real verb (role/fields files), so the input layer is covered too. Caveat: the
  planted-leak probe injects in the test sink, not in the estimator (S2).
- Consumer: NAV decides at d after its mark, fills at d+1's close, first return row d+2
  (strategy_nav_replay.hpp:22-23; loop :1370 `s.decision = t + 2 < end`). `BookScaler::estimate(d)` reads
  `risk_->read(d)` (strategy_risk_target.cpp:140-141). The container path reads `view(d)` and refuses unless
  `as_of == sessions_[d]` (strategy_spo.cpp:797-799). Block d holds returns through d's 22:00 mark and is read at the
  23:00 decision. Nothing after the decision point enters.

**`--delisting-returns`: NO. The R-8 / Y-1 role was not built with it.** Read from manifests and receipts only:
- Y-1's NAV receipts, both the v8 reference `build-equity/mega-nav-v8y-vol-target-L2.0-run/receipt.json` and the P9-B0
  rerun `build-equity/p9-b0-y1-nav-run/receipt.json`, pass `--role build-equity/train-2020-2023-lo3/manifest.json
  --role-sha256 e1c67101...395f4` and `--risk-model build-equity/v8-risk-lo3-v10 --risk-model-sha256 862515d9...cd5a`.
  I re-hashed both pins: the R-8 manifest SHA is `862515d9...`, and its `role.manifest_sha256` is `e1c67101...`.
- That role manifest (universe `linked-operating-v3`) carries `universe.delisting.returns_applied: false`. `--delisting`
  (mark-only, required by v3) was given; `--delisting-returns` was not. The role builder's own text
  (atx-engine/tools/prepare_recent_research.py:37-41, :912) makes `returns_applied` the record of that flag. The later
  `repair` (factor-break-v1) lists `raw_close.f64`, `present.u8`, `member.u8` and `volume.f64` as unchanged.
- So the lane's G-P11 concern (1) does not arise for the role P9 uses, and COV-6's prereg exception is not needed for
  it. Caveats A/B (vendor back-adjusted level; `historical_vintage_verified: false`) are pre-existing role properties.
  They do not change returns, because only ratios are read. Guarding future roles: S9.

### (c) Flag-absent byte identity
**Holds by code reading. The in-tree tests prove new-vs-new only.** Every RiskV11-path edit was checked:
- `recipe_json` returns `recipe_v11_json` unchanged under RiskV11 (strategy_risk_verb.cpp:820).
- `model` = `recipe_id(RiskV11)` = the same literal.
- The manifest refactor (`bias_block`, `seal_json`, `descriptors_used_json`) builds the same objects. nlohmann::json
  sorts keys, so the dump is identical. `eigen_adjustment` / `container` / `family_set` are added only under their flags
  (:1002, :1145, ManifestExtras).
- `family_summary(..., false)` is unchanged.
- BiasHarness V1: same `held_` / `series_` layout, and `padded(q, 3)` equals the old pad.
- Model: Kish / `returns_entered` / `specific_flag` / `eigen_gamma` are computed but never written to a V1.1 file, and
  S2b is gated at strategy_risk_model.cpp:989.
- `--max-bytes` reaches the kernel only under CovV1 (verb :1112).

`CovRecipe.FlagsAbsentOutputsUnchanged` compares flag-absent runs against flags spelled at their defaults and against
`--emit-container` (manifest minus `container` / `files["model.atxcov"]`). `CovEigen.RecipeV11IsBitIdenticalToToday`
proves V1.1 ignores the S2b constants. Neither compares with base bytes. No existing gtest pins a verb-output digest,
so the base-anchored proof is root's real-data step 2 (S1).

**"Unchanged filter" claim: confirmed.** The 104/104 log carries the exact filter
(`RiskWls.*:...:NavV7Hook.*`), the binary is from the code-final clean build (`p9-cov-e`), and the lane changed none of
those suites' files: `strategy_risk_model_test.cpp` and `strategy_spo_fixture.hpp` are not in the diff.

## Findings
`path:line | tag | problem | fix`

- **R1** `atx-impl/tests/strategy_cov_model_test.cpp:483-499` | **Required** | `EigenFamilyInBandOnAPlantedModel`
  asserts no band: pooled b in (0.5, 3) and b < b_v11. The brief requires the eigen family in band on a planted model
  (brief-COV.md:150, cov-design.md:561), and the relative check passes for any small-eigenvalue inflation (see (a)). |
  Pick one of two fixes:
  (i) Assert the band. Under atx-cov-v1, the share of eigen series with b inside the harness's band (USE4 or
  kurtosis-widened) must be at least the nominal level and above atx-risk-v1.1's share. Use a planted panel where the
  band can hold: constant membership, so no churned constraint direction, and the variance half-life equal to the
  correlation half-life, so S2b's simulated length matches the estimator. Any rank sitting at the eigenvalue floor
  should be excluded and counted.
  (ii) Show with a per-rank b table (V1.1 against CovV1) that the excess comes from the constraint/floor rank and the
  HL mismatch, not from the kernel, and take a PM ruling to rename the test to the relative claim.
  Keep the 2.056 history in the report either way.
- **R2** `atx-engine/src/risk/cov_container.cpp:218` (also :220, :622, :709) | **Required** | Bounds checks on
  file-supplied offsets and sizes are not overflow-safe:
  - `(bytes.size() - e.offset_in_block) / size` underflows when an array offset lies past the block, so the check
    passes and `typed<T>` forms a span outside the block (UB).
  - `previous_end` can overflow.
  - `section_table_offset + count * 64` can wrap.
  - `next_block += e.block_bytes` can wrap.

  Accidental corruption is caught by the hashes first (blocks are hashed before parse; the root covers the index). But
  a hash-consistent malformed file (a writer bug, or any file opened without a pin, e.g. `cov-info`) and the public
  `parse_cov_block` reach UB instead of the documented ParseError ("array table in bounds"; agent.md §0, §4 "validate
  at the boundary"). | Compare against the remaining length (`offset <= size && count <= (size - offset) / elem`;
  `section_table_offset <= data_end && count <= (data_end - offset) / 64`;
  `e.block_bytes <= blocks_end - next_block`). Add one `parse_cov_block` test with an array offset past the block, and
  one with a wrapping section-table offset.
- S1 `atx-impl/tests/strategy_cov_model_test.cpp:154`, `strategy_cov_store_test.cpp:67` | Suggested | The names
  overclaim identity "to today": both compare new code with new code. | Rename (for example
  `RecipeV11IgnoresS2bConstants`), or pin a small synthetic verb run's file digests computed at `ad406718`. The real
  proof stays root step 2.
- S2 `atx-impl/tests/strategy_cov_fixture.hpp:285`, `strategy_cov_model_test.cpp:103` | Suggested | The planted leak
  is applied to the sink's f32 exposures after the estimator. It proves the comparator fails, but it is not "a
  deliberately leaky estimator" (cov-design.md:328). A constant shift or scale of a raw descriptor would be removed by
  standardisation, so the leak must be per-name. | Add an input-side per-name leak: each name's value descriptor
  replaced by its own full-sample mean in the panel. It flows through `run_risk_model` and survives standardisation.
- S3 `atx-impl/src/strategy_risk_model.cpp:193-198`, `strategy_risk_verb.cpp:1112` | Suggested | A kernel
  `OutOfRange` (workspace budget from `--max-bytes`) is counted as "refused" together with non-PD refusals. A small
  `--max-bytes` would make an "atx-cov-v1" run equal to v1.1, visible only in a count. | Fail the run on a budget
  refusal, or count it separately (`refused_budget`).
- S4 `atx-impl/src/strategy_spo.cpp:731` | Suggested | I-4's "container reaching the seal without an unseal record"
  is not refused on the read path (`flag_unsealed` is written but never checked). The impact today is low: P9 roles end
  2023-12-29. | Refuse when `last_session_ns >= seal` and `!unsealed()`.
- S5 (root note) | Suggested | Debug cost of S2b on TRAIN. The lane measured about 0.1 s/session on its synthetic data
  (K about 15, T <= 300). On TRAIN, K is 62 and the Kish count about 1,000-1,450, roughly 30-110x more per session over
  1,405 sessions. My estimate is hours in Debug against the 600 s step-5 limit. | Plan for the cov-design §6 Q4
  Release ruling. Release-vs-Debug byte identity of `eigen_adjust_v2` (Eigen GEMM blocking) may itself fail; check it
  first on the synthetic fixture.
- S6 new files (`cov_container.cpp` 11 lines, `risk_cov_container_test.cpp` 16, `cov_container.hpp` 4, impl tests
  42, verb/model additions 15) | Suggested | About 89 lines exceed the 100-column limit named in brief rule 1. The owning
  impl files already had some at base (13 / 16); the new engine files had none to inherit. | Reflow.
- S7 `atx-impl/tests/strategy_cov_model_test.cpp:516-540`, `strategy_risk_model.cpp:1403-1457` | Suggested | The
  test checks that the books held are a subset of the complete names, not that they equal them. Woodbury minvar /
  optimized weights are never compared with a dense V^-1. An LLT failure silently skips the extended books for that
  session. | Assert held == complete set. Add a small-N dense oracle (1e-10). Count sessions where the extended books
  could not be formed.
- S8 `atx-impl/src/strategy_risk_model.cpp:844` (PM note, not a lane defect) | Suggested | The S2b simulated length
  (Kish of HL 504) overstates the effective sample of F's variances (HL 84), so atx-cov-v1 under-corrects by design.
  This is fixed in the brief; changing it means a new recipe id (COV-5). | Record it for D-COV's reading of the eigen
  family, and as a P10 recipe candidate (simulated length = Kish of the variance half-life, or simulate the EWMA
  estimator itself).
- S9 `atx-impl/src/strategy_risk_verb.cpp:115` | Suggested | The verb neither records nor checks the role's
  `universe.delisting.returns_applied`. A future role built with `--delisting-returns` would feed imputed post-T
  information silently (lane trace item 3). | Copy the flag into the manifest / container META, and refuse unless
  acknowledged.

Cross-lane edits: minimal and as briefed. `atx-engine/CMakeLists.txt` gets one source line. In
`atx-impl/tests/CMakeLists.txt`, two lines are appended at the tail of the strategy-target list (the closing paren
moves). `strategy_spo.cpp` gets two include lines outside the section, which the report lists; all other spo edits
are inside the RiskStore section. No forbidden file is touched (nav_replay / nav_v7 / risk_target / vol_target /
spo_v3 / book / eigen_adjust / atx-impl CMake). There is no Python change and no expected hash edited.

Container integrity (checked):
- The root covers `header[0,224)`, the section table, META, names, index and hashes. Blocks are covered through
  BLOCK_HASHES (a two-level Merkle tree).
- Truncation is refused through `file_bytes`, the trailer and the complete flag.
- A flipped byte is refused by Full at open and by Lazy on first view (sticky). A root pin mismatch is refused as
  InvalidArgument.
- Overlap, misalignment, unknown-required and major>1 are refused; an unknown optional section or array is ignored.
- The writer appends to `.partial` with exclusive create, then `_commit` / `fsync`, then renames.
- Full verification uses a fixed partition and reports the smallest failing block, independent of the worker count.

`--max-bytes` handling (checked): the mapping bound is 64 GiB in the store and the tools, refused by
`Mapping::map_file_ro`. The kernel budget comes from `--max-bytes` only under CovV1 (default 268,435,456); it is
recorded with its source in the manifest and kept out of the recipe hash. The workspace check cannot underflow (it
short-circuits). The gaps are S3 and R2.

Eigen numerics (checked):
- A non-PD, non-finite or asymmetric input is refused by the kernel and the block is kept and counted.
- n_eff < K+1 gives ShortHistory.
- The floored near-singular block is adjusted with gamma^2 > 0, so the result is PD (`ResultIsPositiveDefinite`).
- The gammas are recomputed on the same `symmetric_eig` the kernel uses, so they stay consistent within degenerate
  eigenspaces.
- The structural border and PD scaling, and the VRA, read the adjusted block (`VraReadsTheAdjustedPrior` has teeth).

## Spec compliance (brief items, carry COV, rulings)
- Brief items 1-8: met. Item 7 bench: written, not compiled (`ATX_BUILD_BENCH` is off in the lane tree, allowed).
  I read it for compile hazards and found none.
- All 18 engine and 26 impl gtests named in the brief are present.
- Carry COV-1/2/5 (0 trials, constants a = 1, 64, seed 7, HL 504): met.
- COV-3 ownership: met.
- COV-4 trace and truncation / perturb / leak tests: met (S2).
- Cross-lane edits: met.
- C1 fixtures reused: met (`CovStore.VolAndRiskTargetRecordsUnchanged`, both laws).
- Root-pin refusal: met.
- Rulings COV-1 (order), COV-2 (`--max-bytes` into the kernel, default when absent) and COV-3 (MRAD per series; QLIKE
  pooled): met.
- COV-5: **not met** (R1).
- COV-6: answered under (b).

## What root must check at merge
1. Re-review after the fix round (R1, R2), at the new SHA.
2. The brief's root steps 1-7 as written. Step 2 is the only base-anchored flag-absent proof (S1). For steps 3-4,
   `cov-diff --legacy` compares all four files only with `--emit-exposures all`, which R-8 has.
3. Step 5: expect a Debug wall far above 600 s (S5). Take the Release ruling, with the Release-vs-Debug synthetic
   identity check first.
4. The bench TU has never been compiled; compile it at step 6.
5. G-P11: tick it. The as-of trace holds, and the P9 role has `returns_applied: false`, so no prereg exception is
   needed. Record S8 and S9 for D-COV and P10.
6. `atx-impl/tests/CMakeLists.txt` tail: keep both C2's and COV's lines.

## Checked
- [x] `.agents/cpp/agent.md` §10 applied to the diff. Findings: R2 (UB on malformed input) and S6 (100 columns).
  Warnings were clean in the lane's builds.
- [x] The diff stays inside the brief's files-in-scope (plus the two listed include lines and the CMake tails).
- [x] The report's evidence matches its claims: receipts' SHAs and dirty state, log filters and counts, and the
  `ba2e2de6..18049a77` code delta. The Python guard pytest was re-run (exit 0).
