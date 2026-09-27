# Task T1 — IC runner: candidate signal cache and pinned composition weights

Owner worktree: `C:/atx-wt/pool-4`, branch `feat/mega-alpha-runner-cache-20260926` (base `5c9cbaed`).
Files you own: `atx-impl/src/strategy_ic_runner.hpp/.cpp`, `atx-impl/src/strategy_ic_composition.hpp/.cpp`,
`atx-impl/tests/strategy_ic_runner_test.cpp` (and a new sibling test file if cleaner). Do not touch
CMakeLists (root owns registration; if you add a new test TU say so in the report), public engine
headers, or other modules.

## Why

The mega-alpha objective needs many more DSL subalphas and fast iteration on how they are combined.
Today every run re-evaluates every DSL program in the VM (TRAIN 48 candidates: VM 30 s of 95 s), and
composition is hard-coded to fixed equal 1/48 weights with TRAIN-fit signs. Real runs are capped at
180 s and host RAM is tight, so work must be incremental: a run that stops part-way must leave reusable
progress, and composition research must reuse evaluated signals instead of re-running the VM.

## Requirements

1. `--candidate-cache DIR` (optional; default off = today's behaviour, bit-identical outputs).
   - After the VM evaluates a candidate for a role, write its exact raw (UNoriented, pre-composition)
     signal as date-major little-endian f64, identical to the VM output the runner already uses
     (NaN wherever the VM output is non-finite), to `DIR/<role-manifest-sha256>/<candidate-id>.f64`,
     with a sidecar `<candidate-id>.json`: schema `atx.dsl-candidate-signal/v1`, candidate id,
     `dsl_sha256`, library sha256, role manifest sha256, role name, dates, instruments, byte count,
     payload sha256, eval mode, and the engine source identity the runner already records.
   - Write atomically (temp file in same dir + rename). Never overwrite an existing file.
   - On a later run with the same DIR: if a sidecar exists whose dsl sha, role manifest sha,
     geometry and payload sha256 (verified by hashing the payload on load) all match, load the payload
     instead of evaluating the VM and log `IC cache-hit <id>`; otherwise, if a sidecar exists but
     mismatches, fail loudly (do not silently recompute over it). Missing entry -> evaluate + write.
   - Downstream IC, orientation, composition, planned targets and saved blend must be bit-identical
     between: no cache, cold cache, warm cache.
   - Memory: one candidate payload buffer at a time (reuse); count it in the existing admission budget.
     No per-candidate retention. Hashing streams (no second full copy).
   - Consequence to preserve: a run stopped by the external guard after K candidates leaves K cached
     signals, so a rerun resumes cheaply. Log cache hits/misses and cache seconds in the summary
     stage timings (`cache_load`, `cache_write`).
2. `--composition-weights PATH --composition-weights-sha256 SHA` (optional; default unchanged).
   - Pinned JSON: schema `atx.dsl-composition-weights/v1`, `library_sha256`, and an object mapping
     every library candidate id -> finite weight >= 0. Unknown id, missing id, negative/non-finite
     weight, wrong library sha, or hash mismatch -> error before loading payloads.
   - Replaces the fixed equal family/within-family weights; signs still come from TRAIN orientation
     (or the pinned orientations artifact in validation-only mode). Weight 0 = evaluated for IC but
     contributes nothing. Missing-component behaviour unchanged (no redistribution).
   - Record the weights file sha in recipe.json, summary and saved-blend manifest (canonical hashes
     must change when weights are supplied, and must NOT change when absent).
   - A weights file equal to the default 1/48-per-candidate values must reproduce the default blend
     bit-identically (same arithmetic order) — make sure the code path guarantees that.
3. `--plan-only` must validate the new options' pins without loading role payloads.

## Constraints

- House style: read `C:/atx-wt/pool-4/.agents/cpp/agent.md` first. Private CPP only; keep
  `strategy_ic_runner.hpp` changes to new config fields.
- Do NOT build or run anything heavy: root alone compiles and runs real data. Write careful code;
  root will compile and report errors back to you. You may read anything.
- Not TDD: implement production code first, then add focused postimplementation fixtures on the
  existing tiny synthetic fixtures: (a) no-cache vs cold vs warm cache bit-identity of summary metrics,
  planned targets and saved blend bytes, plus cache-hit log; (b) tampered payload -> loud failure;
  (c) pinned equal weights == default blend bytes; (d) non-equal weights change the blend and the
  recipe hash; (e) bad weights files refused before payload load.
- No change to numerics, formulas, sign rules, orders or FP when the new options are absent.
- No subagents. Commit in pool-4 with clear messages ending with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`. Do not push.

## Report

Write the full report to `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T1-report.md`
(what changed, file:line, fixtures added, anything uncertain, exact commit SHAs). Return only:
status (DONE / DONE_WITH_CONCERNS / BLOCKED / NEEDS_CONTEXT), commit SHAs, one-line summary, concerns.
