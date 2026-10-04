# Lane COV hand-off (owner stop, 2026-10-03)

> **Superseded (resumed and finished the same day):** every step below is done; the state of record is
> `task-COV-report.md` (commits after `16233715`, builds `p9-cov-b..e`, 18 + 26 named gtests green, evidence in
> `cov-evidence/`). This file is kept as the resume map that was followed.

- **Pool** `C:/atx-wt/pool-24` (fresh lease; run id `p9-cov-20261003`, heartbeat `p9-cov-hb`, still held).
- **Branch** `feat/p9-cov-20261003`, **base** `ad406718` (all eight wave-1 lanes merged, C1 included).
- **Head** the WIP commit `6fa702b2` plus the commit that adds this file (see `git log`).
- **Contract:** `wave2-lane-dispatch.md` with the dispatcher's overrides (LANE = COV; brief section "Lane COV" only;
  rulings COV-1..8 + G-P11; builds allowed only through `scripts/research-build.ps1`, tags `p9-cov-<letter>`,
  target-scoped, one at a time, memory gate honoured; synthetic data only; C2 shares the
  `atx-impl-strategy-target-tests` list: append at its tail).

## State per brief deliverable

| # | item | state |
|---|---|---|
| 1 | Container library (engine) | **done (code), test-compile pending.** `atx-engine/include/atx/engine/risk/cov_container.hpp`, `atx-engine/src/risk/cov_container.cpp`; CMake line added beside `specific_risk.cpp`. `cov_container.cpp` compiled clean under /W4 /WX in build `p9-cov-a` ([97/279]). |
| - | Engine gtests (18 `RiskCovContainer.*`) | **written** in `atx-engine/tests/risk/risk_cov_container_test.cpp` (auto-globbed into `atx-engine-risk-tests`); compile/run result: see "Builds" below. |
| 2 | Recipe `atx-cov-v1` (impl) | **done (code, not compiled).** `strategy_risk_model.{hpp,cpp}`: `RiskRecipe {RiskV11, CovV1}`, config fields `recipe`, `eigen_simulations 64`, `eigen_amplification 1.0`, `eigen_seed 7`, `eigen_max_bytes`; `eigen_adjust_block` (public, testable), `eigen_session_seed` = splitmix64(seed XOR ns), `EigenOutcome`; Kish n_eff tracked in `Model::update_vra` (HL = `correlation_halflife` 504, sessions with a finite market factor return); S2b call inserted in `fully_observed_forecast` after the floor, before the prior/cov writes (so the structural border, class priors and VRA read the adjusted block); `RiskDay` gains `eigen`, `eigen_effective`, `eigen_gamma`, `vra_factor_applied`, `returns_entered`, `specific_flags`. Manifest side (recipe block `eigen_adjustment`, `model` atx-cov-v1, counts) **not started** (verb). |
| 6 | Extended bias families | **partial.** Header done (`BiasFamilies`, `optimized_portfolios 32`, `optimized_seed_salt`, `qlike`, `mrad`, `Eigenportfolios` state). Cpp done: constructor series layout, `realize` (minvar/optimized settle, `realize_eigen`), `form` hook. **Missing definitions:** `BiasHarness::form_extended`, `qlike`, `mrad`. Design for `form_extended` below. Verb summary keys (qlike/mrad per family under extended) not started. |
| - | Container rows (impl) | **declared only** in `strategy_risk_model.hpp`: `CovDenseRow`, `CovBlockScratch`, `cov_block_of`, `scatter_cov_block`, `cov_diag_names`, `LegacyComparison`, `compare_container_to_legacy`, `dispatch_cov_info`, `dispatch_cov_diff`. **No definitions yet: atx-impl does not link.** |
| 3 | Truncation + eigen tests, `strategy_cov_fixture.hpp` | not started |
| 4 | Writer wiring (verb flags, ContainerSink, manifest `container`, cov-info / cov-diff verbs, `tools/equity_strategy_risk.cpp`) | not started |
| 5 | RiskStore container read path (`strategy_spo.{hpp,cpp}` store section) | not started |
| 7 | Bench `atx-engine/bench/risk_cov_container_bench.cpp` | not started |
| 8 | README rows + `task-COV-report.md` | not started |
| - | `atx-impl/tests/CMakeLists.txt` two lines (`strategy_cov_model_test.cpp`, `strategy_cov_store_test.cpp`) at the tail of the target-tests list | not started |

## Next steps, in order

1. **Define the missing impl functions** (in `strategy_risk_model.cpp`):
   - `qlike(z)` = mean(z^2 - ln z^2) over finite z with z != 0; NaN when none. `mrad(z, w)` = mean |b - 1| over the
     finite values of `rolling_bias(z, w)`; NaN when none.
   - `BiasHarness::form_extended(day)`: (a) forecast block = factors with finite `cov[k,k]` (all entries among them
     are finite by construction; refuse to form if not); `Eigen::SelfAdjointEigenSolver` -> `eigen_` (factors,
     column-major vectors `vectors[k*m + a]`, ascending values; live only if all values > 0). `realize_eigen` already
     reads `vectors[k * m + a]` = component a of eigenvector k. (b) universe = `day.eligible`, slot != no_exposure,
     finite positive specific, and every loaded factor (market, `industry_factor(slot)`, each style with z != 0) has a
     finite diag; < 2 names -> nothing formed. (c) Woodbury on the block: `F^-1` by LLT, `C = F^-1 + X' D^-1 X`
     (dense N_u x m X), `V^-1 v = (v - X C^-1 X' D^-1 v) / D`. minvar: `a = V^-1 1`, w = a / sum a ->
     `held_[random_]`. optimized q: `b = V^-1 alpha_q` (alpha from `optimized_scores_`), w = b - (sum b / sum a) a,
     scaled to gross 1 -> `held_[random_ + 1 + q]`; `apply_forecast(h, forecast_vol(day, h))` for each.
     Series index map: eigen k at `random_ + factor_count + k`; minvar `random_ + 2*factor_count`; optimized q at
     `random_ + 2*factor_count + 1 + q`; book last. held_: random..., minvar, optimized..., book last.
   - `cov_block_of` / `scatter_cov_block` / `cov_diag_names`: the row rule as documented in the header (lossless:
     a row for slot != 255, a specific whose bits != the default quiet NaN, or any style f32 bits != 0; priced =
     slot < 50 and finite positive specific; GROUP = slot + 1; refuse a slot in [50, 255)). Scatter defaults: slot
     255, specific `quiet_NaN()`, styles +0.0f; f64 exposure files narrowed with `static_cast<f32>`.
     DIAG (32): lambda2_factor, lambda2_specific, bias_factor, bias_specific, structural_scale, r2, eigen_effective,
     regression_rows, active_industries, structural_names, specific_names, structural_factors,
     unforecast_exposed_factors, residual_names, fenced_values, unscaled_descriptors, max_specific_variance,
     structural_sigma_lower, structural_sigma_upper, structural_bounded_names, structural_clamped_names,
     eigen outcome code, rest NaN.
2. **Verb** (`strategy_risk_verb.cpp`): flags `--recipe`, `--emit-container` (valueless: handle before the generic
   `i + 1` value path in the parser), `--bias-families`; `cfg.recipe` from the flag (cfg is `const RiskModelConfig
   cfg{}` today at `run`); `recipe_json(cfg)` adds `eigen_adjustment {method "menchero-wang-orr simulated (USE4
   B7)", amplification 1.0, simulations 64, seed 7, simulated_length "kish effective count of the HL-504 weights",
   seed_rule "per session"}` and `model` = `recipe_id(cfg.recipe)` **only under CovV1** (the V11 JSON must stay
   byte-identical); top-level manifest `model` likewise; a CovV1-only manifest block counting EigenOutcome per
   session. `ContainerSink`: narrow `d.styles` to f32 exactly as `FileSink` does, build `CovDenseRow` (flags:
   forecast, structural_factors > 0, eigen Adjusted, vra_factor_applied; history_first = first session with
   `returns_entered`, else the session), `cov_block_of`, `CovContainerWriter::append`; writer created in `run`
   before `run_risk_model` at `dir / "model.atxcov"` (`.partial` until finalize). Spec: groups 50, dense 11,
   f32 exposures, role pin raw bytes (`cov_parse_hex`), `recipe_json = recipe.dump()`, `inputs_json` = canonical
   dump of {fields pin, fields_used, descriptors_used, book pin}, META = canonical dump embedding both + producer,
   seal, diag names, row rule. Manifest `container {file, format "atx.cov-container/1.0", bytes, file_sha256,
   root_sha256}` + a `files["model.atxcov"]` entry only with the flag; manifest still last. `--bias-families v1`
   keeps `bias.json`/`bias_summary.json`/`bias_harness` bytes. Then `dispatch_cov_info`, `dispatch_cov_diff`,
   `compare_container_to_legacy`, and the two verbs in `tools/equity_strategy_risk.cpp`.
3. **RiskStore** (`strategy_spo.hpp:221-266`, `strategy_spo.cpp:629-777`): member
   `std::shared_ptr<const atx::engine::risk::CovModelFile> container_` (forward-declare the class in the header);
   no `container` key -> today's code path untouched; with it: map Full at the manifest root, role pin, K/G/S
   62/50/11, date count, diagnostics.csv sessions + forecast flags vs the index, skip the four legacy payload
   hashes; `read(d)` = `scatter_cov_block` then the legacy rules (NaN cov -> 0 counted, f32 -> f64, non-finite -> 0).
4. Tests (impl): `strategy_cov_fixture.hpp` (planted panel with two IPOs, two delistings, churn, missing
   descriptors, a thin industry; `truncate_after`, `perturb_after`, `add_future_instruments`; an in-memory sink
   = `cov_block_of` + `encode_cov_block`; a leaky test-only sink that standardises one style by its full-sample
   mean for `DetectsAPlantedLeak`; a legacy->container converter for `write_risk_model` stores, used by
   `CovStore.VolAndRiskTargetRecordsUnchanged`), `strategy_cov_model_test.cpp`, `strategy_cov_store_test.cpp`,
   the test names of the brief. Use a small `RiskModelConfig` (short windows, `structural_history` ~30,
   `min_factor_history` ~20) so S2b runs early and fast: the engine kernel is /Od in Debug.
5. Bench, README rows, report, CMake tail lines; builds `p9-cov-b` (atx-engine-risk-tests again, then run
   `--gtest_filter=RiskCovContainer.*`), `p9-cov-c` (atx-impl-strategy-target-tests), `p9-cov-d`
   (atx-equity-strategy-risk).

## Builds

- `p9-cov-a` = `atx-engine-risk-tests` (equity-dev tree `build-equity/`, configured once in this pool). First
  attempt refused by the memory gate (commit 1633 < 2500), retried by a waiting script
  (scratchpad `gated-build.ps1`), admitted 17:14; 460 s, 222 TUs, **exit 1**: `cov_container.cpp` and every
  other TU compiled clean; the only error was `risk_cov_container_test.cpp(247)` "unused function
  'parse_error'" (the `co::Status` overload), **fixed in the hand-off commit but not rebuilt**. Next build tag:
  `p9-cov-b` (receipt `p9-cov-a` exists, so that tag is spent). No test has been run yet.

## Decisions (and why)

- Engine container has no JSON dependency (atx-engine's nlohmann include path is per-source): META, recipe and
  inputs are opaque canonical texts from the caller; recipe_sha256 / inputs_sha256 = SHA-256 of those texts.
- Every block is padded to a multiple of 4096 so its hash covers every byte; the reader also requires every
  byte outside the sections (header padding, gaps, pre-trailer) to be zero and the blocks to tile BLOCKS exactly.
- Section counts: META/NAMES count = bytes (utf8), DATE_INDEX count = D (64 B records), BLOCK_HASHES count = D
  (32 B), BLOCKS count = D (bytes tiled by the index). FACTOR_NAMES = K names, each NUL-terminated.
- `at_or_before(s)` = the last block with as_of <= s (cutoff <= as_of <= s): never a block from after s in
  either sense. `first_difference` compares geometry, sessions and block hashes only (a truncated role is
  another role; recipe/role/inputs are not compared).
- Full verification: fixed contiguous partition over `workers` (0 -> min(8, hardware)); the smallest failing
  block is reported, so the answer is worker-count invariant.
- S2b "fewer than K+1 effective observations" is a pre-check `n_eff >= m + 1` (else ShortHistory, block kept);
  any kernel error -> Refused, block kept; both counted per session via `RiskDay::eigen` (manifest count to do).
  Gammas are diagnostics: sqrt(u_k' F_adj u_k / d_k) on the input's eigenbasis (eigen_adjust_v2 does not return
  them and is forbidden to edit).
- `eigen_max_bytes` defaults to the kernel's 268,435,456 (proposal: the verb leaves it there; brief says
  `max_bytes`, open question 2).

## As-of trace so far (COV-4; the reviewer must re-check, files at base `ad406718`)

1. Role clock: `atx-engine/tools/prepare_recent_research.py:82` `CLOCK = "modeled-session+22h-mark+23h-decision-v1"`
   (row t's close is session t's close, marked 22:00 UTC; the decision is 23:00 UTC); membership at t is
   computed before t's bar (`:481-482`); field rows are visible at t when their clock is before date(t-1)
   22:00 UTC (`:47`; fields' own clock in `prepare_research_fields.py`, not yet re-read).
2. Producer: block t is emitted at the end of session t's pass of `run_risk_model`
   (`strategy_risk_model.cpp:1041-1063` at base): `update_prices(t)` uses `interval_return(p, t, i)` = close[t] /
   close[t-1] - 1 (`:541-549`), regress t on X_{t-1}, VRA/EWMA push of session t, exposures X_t (rows <= t),
   forecast. No statistic over the whole axis is visible in that code (the `Model` vectors sized by `dates` are only
   indexed <= t); the truncation suite (to write) is what proves it.
3. Consumer: NAV replay session loop `strategy_nav_replay.cpp:1367-1371` (`s.decision = t + 2 < end`): phase 1
   MARK/EXECUTE at close t, phase 2 `close_book` -> `plan_decision(..., s.t, ...)` (`:1286-1287`) -> current =
   held / nav_post (`:1036`) -> `plan_weights` (`:1058`) -> `p.leverage->plan(p.x, p.cfg, p.d, ...)` (`:966`) ->
   `BookLeverage::plan` -> `scaler_.leverage(x, d, ...)` (`strategy_risk_target.cpp:191-192`) -> `estimate(d, ...)`
   (`:114`) -> `risk_->read(d, slice_)` (`:141`). The planned weights become working orders filled at EXECUTE of
   session d+1's close (`strategy_nav_replay.hpp:17-21`; EXECUTE `strategy_nav_replay.cpp:736`), first return row
   d+2. So the decision at d (23:00 UTC) reads block d, built from closes through the 22:00 mark of d and fields
   visible before date(d-1) 22:00; first traded at d+1's close. Remaining to show: that the role's adjusted
   close / cumulReturnFactor at row d embeds nothing after d (outside COV's code; flag to the reviewer).

## Open questions for the PM

1. The COV lane as briefed (L effort, 8 deliverables, ~45 gtests) did not fit one session: split the remainder
   (e.g. COV-b: verb + store + tests; COV-c: bias families + bench + report)?
2. `eigen_max_bytes`: keep the kernel default (268 MB) or wire the verb's `--max-bytes`?
3. MRAD reading: implemented as "per series, mean |b - 1| over its rolling-window b, then the family mean" (windows
   63 and 252); confirm "asset-level" means per-series, not a single-stock family.

## Traps

- `atx-impl` does not link at `6fa702b2` (declared-not-defined functions listed above); build `atx-engine-risk-tests`
  alone until step 1 lands.
- The Windows mapping opens files `FILE_SHARE_READ`: destroy `CovModelFile` before deleting / rewriting its file
  in tests.
- The engine kernel (`eigen_adjust.cpp`) is /Od in the Debug tree while `strategy_risk_model.cpp` is /O2:
  keep CovV1 test panels small (K_keep ~10, < 150 sessions) or tests take minutes.
- Flag-absent identity: add nothing to `recipe_json`, the manifest or `bias_summary.json` under V11 / v1 / no
  container; the manifest's key order is sorted, so a `container` key is the only diff.
- research-build refuses a reused tag only once its receipt exists (a gate refusal writes none).
- `strategy_spo_fixture.hpp` is frozen (spo pin digests): do not edit it; convert its stores instead.
