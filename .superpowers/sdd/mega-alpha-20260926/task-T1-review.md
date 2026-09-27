# Task T1 review — IC runner candidate signal cache + pinned composition weights

Package: `review-T1.diff` (af8c38ee feat, 445e828d test; base e44a44d5). Source line numbers are from
`C:/atx-wt/pool-2` HEAD. The T1-owned files there have not changed since af8c38ee/445e828d.

### Spec Compliance

- ❌ Issues found. They come from the global constraints; brief requirements 1–3 and fixtures (a)–(e)
  are all implemented:
  1. **The cache key has no engine/VM semantics identity (C1).**
     - `atx-impl/src/strategy_ic_runner.cpp:395-397` states "A changed VM needs a fresh DIR".
     - `cached_payload_sha` (`:426-441`) matches only schema, id, dsl sha, role sha, eval_mode label,
       layout, geometry and payload sha.
     - `engine_git_sha` is recorded (`:557`) but never compared.
     - This violates "key must cover … engine/VM semantics version". The brief's own match list
       (task-T1-brief.md:27-29) left it out. That is a brief/constraint conflict; the constraint wins.
  2. **Pinned weights are not bound to TRAIN (I1).**
     - `frozen_train` (`:127-137`) deliberately lets a validation-only run pin any weights, or none,
       with no link to the frozen TRAIN run.
     - `composition_weights` (`:367-393`) checks library identity only.
     - `strategy_ic_runner_test.cpp:722-750` treats both mismatch directions as supported.
     - This violates "pinned weights … TRAIN-fit only" and the lane-contract selection-hygiene rule.
- Verified against the diff:
  - **R1 cache: ✅**
    - Layout `DIR/<role sha>/<id>.{f64,json}`: `:739-745`.
    - Sidecar has every brief field plus family, semantics, vm_workers and engine_git_sha: `:551-557`.
    - Temp file plus hard-link publish that never replaces an existing file: `:479-519`. This is
      stronger than rename, which replaces on Windows.
    - Never overwrites; adopts an orphan payload only if byte-identical: `:522-540`.
    - Hit path: sidecar identity check plus payload SHA hashed while loading: `:456-477`, `:661-667`.
    - A mismatched sidecar is a loud error (`:439`); a miss evaluates and writes (`:674-702`).
    - One reused buffer: `:674`, and released after the loop.
    - Hashing streams (no second copy).
    - Per-candidate and role cache_load/cache_write, hits and misses in the summary: `:802-806`,
      `:857-862`.
  - **R2 weights: ✅**
    - Options must be given as a pair: `:873`.
    - Refused before admission (`:367-393`, `:878`):
      - schema, library sha, unknown id, missing id
      - negative, non-finite or non-numeric weight
      - duplicate key, parse overflow, hash pin
    - Values stored verbatim: `strategy_ic_composition.cpp:106`.
    - Pin recorded in recipe (`:98-105`), manifest (`:242-259`) and summary (`:930`). The key is
      absent, not null, when no weights are given.
  - **R3 plan-only: ✅**
    - Weights and cache preflight run at `:878-879`, before `admit`.
    - `cache_plan` stats payload files without reading them (`:574-588`).
  - **Fixtures: ✅** (a) `test:449`, (b) `:545`, (c) `:592`, (d) `:638`, (e) `:665`, plus resume
    (`:512`) and validation-only composition (`:709`).
- ⚠️ Cannot verify from diff:
  - **Pinned-default equals default on the real 48/96-candidate library.**
    - Proven only on the 4-candidate fixture (`test:592`).
    - By construction it holds only if the file stores the correctly rounded double of `1.0/(F*n)`
      (`strategy_ic_composition.cpp:110`). Python `repr(1/48)` qualifies; `(1/F)/n` may not.
    - Root check: one all-hits TRAIN run with an equal-weights file, then compare
      `train_combined.f64` bytes.
  - **T7 extra fields must extend this key** (task-T7-brief.md:37-40). Today the key covers only
    role-manifest payloads.
  - **Report concern 1 (replay refuses pinned blends) is already resolved elsewhere.**
    `strategy_target_replay.cpp:288-310` now admits the pinned-weights semantics string and requires
    the pin.

### Checks run (named risk → what was checked)

- **Could the zero-weight skip in `add()` (`strategy_ic_composition.cpp:138`) drop side effects that
  do not depend on the weight?** No.
  - The loop body (`:139-148`) contains only weight-scaled accumulations.
  - `eligible_names` is set elsewhere.
  - The accumulator starts at +0 and cannot become -0, so the skip is bit-identical.
- **Does canonicalizing the cold path in place (`:549`: Inf/−NaN → quiet NaN) break no-cache == cold?**
  No.
  - IC admits only `std::isfinite` cells (`atx-engine/src/factory/ic_screen.cpp:248`).
  - Composition does the same (`strategy_ic_composition.cpp:141`).
  - `finish()` reads only the blend.
- **Is the role-manifest SHA a sufficient role-payload key?** Yes.
  - `read_strategy_role` verifies every payload file SHA against the manifest receipts
    (`atx-engine/src/data/strategy_data.cpp:37-63`).
  - `score_role` checks that the manifest SHA equals the pin.
- **Could `engine_git_sha` simply be matched?** Not safely (this feeds C1).
  - It is baked at configure time only, with `-dirty` and `unknown` fallbacks
    (`atx-impl/CMakeLists.txt:82-111`).
  - So it goes stale across rebuilds without a reconfigure, and it says nothing about the preset or FP
    flags.
- **Does "hashing streams" hold?** Yes.
  - `co::sha256_file` reads in 64 KiB chunks (`atx-core/src/sha256.cpp:157-175`).
  - The load path hashes each slice in place as it lands (`:463-470`).
- **Is workers, an unmatched cache parameter, safe?** See M1. Serial-vs-2-worker parity is asserted
  only at the IC and planned-target byte level (`test:158-204`).

### Strengths

- **The load path hashes exactly what it hands downstream**, slice by slice into the reused buffer
  (`:456-477`). There is no time-of-check/time-of-use gap between verifying and using, and no second
  copy.
- **Publication holds up under crashes and concurrent writers** (`:479-571`):
  - unique partial files;
  - hard-link publish that never replaces;
  - payload published before the sidecar, and the sidecar acts as the commit record;
  - an orphan payload is adopted only when byte-identical;
  - a sidecar race is accepted only for the same identity and SHA.
- **In-place quiet-NaN canonicalization makes cold == warm by construction.** Downstream `isfinite`
  gating keeps no-cache == cold (verified above).
- **The default path is untouched.**
  - Recipe, manifest and summary keys are absent when the options are off.
  - The VM block moved verbatim, including the 050c0efc arena release (`:674-690`).
  - Default weights are always > 0, so the weight-0 branch cannot fire on the default path.
- **Weights parsing is careful** (`:352-393`):
  - duplicate keys are refused;
  - a no-throw parse catches `1e999`;
  - output is in library order;
  - every check runs before admission and in plan-only mode.
- **Tests compare real bytes, not mocks.**
  - Full output-set SHA equality across plain, cold and warm runs.
  - No `IC VM-` line on a warm run.
  - The canonical NaN bit pattern.
  - Resume plus orphan adoption, with an mtime check.
  - Tamper cases assert the file is left untouched.
  - Weight refusals run with role payloads deleted, so any payload read would surface a different
    error (`test:665-707`).

### Issues

#### Critical (Must Fix)

**C1. The cache key has no engine/VM semantics identity.**
`atx-impl/src/strategy_ic_runner.cpp:395-397`, `:426-441`, `:557`.

- **What:** An entry keyed on (DSL sha, role sha, geometry, eval-mode label) is served to any later
  binary. `engine_git_sha` is written but never compared. `eval_mode` is a fixed mode label, not a
  version.
- **Why it matters:** After any VM, compiler or kernel fix, a reused DIR silently serves pre-fix
  signals under `IC cache-hit`.
  - The vec_avg member-mask behaviour flagged in T5 is exactly this kind of change.
  - The recipe hash deliberately excludes the cache (`:98-100`), so nothing downstream would show it.
  - A dev build and a rel-avx2 build (FMA/codegen) can differ in the same way.
  - This is the "stale/wrong signal" the global constraint forbids. Root's single-build real run is
    unaffected.
- **Fix:**
  - Key the path as `DIR/<vm-identity>/<role-sha>/`, so an engine change is a clean miss rather than a
    refusal.
  - `<vm-identity>` should be a build-time digest of the atx-engine alpha compiler/VM/ops sources,
    plus the compiler id and FP-relevant flags. Configure-time `engine_git_sha` alone goes stale.
  - Refuse `--candidate-cache` when the identity is unknown.
  - Add a fixture: seed an entry under a foreign identity and assert a miss or a refusal, never a hit.

#### Important (Should Fix)

**I1. Pinned weights have no TRAIN binding.**
`atx-impl/src/strategy_ic_runner.cpp:127-137` (`frozen_train`), `:367-393` (`composition_weights`);
`atx-impl/tests/strategy_ic_runner_test.cpp:722-750`.

- **What:** A validation-only run accepts any weights file, or none, whatever the frozen TRAIN run
  used. The file itself is checked only for library identity.
- **Why it matters:**
  - Nothing prevents N validation-only runs with different weight files against one frozen TRAIN
    artifact. That is weight selection on validation data.
  - The deliverable ("admitted, signed and weighted on TRAIN only … frozen, then run once on
    validation") and lane-contract selection hygiene forbid this.
  - The test suite treats both mismatch directions as supported behaviour.
- **Fix:**
  - Require TRAIN provenance in the weights JSON. T9 already emits the role manifest sha and the
    orientations sha (task-T9-brief.md:33-35).
  - Check that `train_manifest_sha256 == --train-sha256`.
  - In validation-only mode, also require the file's orientations sha to equal
    `--orientations-sha256`. Alternatively, require the pin to equal the frozen TRAIN recipe's pin.
  - Change `test:722-750` to assert refusal of the unbound cases.

#### Minor (Nice to Have)

- **M1. `vm_workers` is recorded but not matched** (`:557`, `:426-441`).
  - A workers=4 entry is served into a workers=1 run whose recipe claims a serial VM.
  - This is safe only because DetPool is deterministic, and that is asserted at the IC/target level
    (`test:158-204`), not on raw signal bytes.
  - Either match it or document the determinism dependency next to the key.
- **M2. Mismatched sidecars are found late** (`:661`). The error surfaces only when the loop reaches
  that candidate, after earlier candidates' VM and IC work. A metadata pre-scan (reuse `cache_plan`)
  at the start of `score_role` would fail fast at negligible cost.
- **M3. Another private copy of the 32-byte hex helper** (`:399-406`). Copies also exist at
  `strategy_target_replay.cpp:251` and `atx-engine/src/data/strategy_data.cpp:~29`. It belongs in
  `atx/core/sha256.hpp`.
- **M4. A `reinterpret_cast` without its `// SAFETY:` note** (`:512`), which house style §9 requires.
  The load path at `:466` has one.
- **M5. Test tolerance looser than the claim** (`test:662`). It uses `ASSERT_DOUBLE_EQ` (4-ULP
  tolerance) while the report claims an exact `2*r` oracle. The arithmetic here is exact, so
  `EXPECT_EQ` would pin the bits.
- **M6. Untested paths:**
  - `cache_preflight` refusals (the path is a file; a device-name id);
  - corrupt sidecar JSON;
  - sidecar role or geometry mismatch;
  - plan-only refusal of a mismatched sidecar;
  - `IcComposition::create` pinned-weight validation at unit level
    (`strategy_ic_composition.cpp:82-84`).
- **M7. Pinned-weight outputs are not self-describing.**
  - With unnormalized pins (e.g. 2/0), the planned-targets `contribution_fraction` column exceeds 1
    (`strategy_ic_composition.cpp:148`, runner `:832`).
  - The applied per-candidate weights appear in no output; only the file sha does. Without the
    external file, NR (Σ w_k τ_k) cannot be computed from the outputs.
  - Record `composition_weight` in each candidate summary.
- **M8. Operational gaps.** None of these fail silently; worth one line in `--help`.
  - No flush to stable storage before the hard-link publish. After a power loss the entry is refused
    loudly and needs manual cleanup.
  - Killed writes leave `.partial` files and nothing sweeps them.
  - Entry paths add a 64-hex role directory, which risks MAX_PATH with long DIRs.

### Assessment

**Task quality:** Needs fixes

**Reasoning:** The cache and weights mechanics are carefully built and tested at the byte level
(verify-on-load streaming, atomic no-replace publication, output identity across no-cache, cold and
warm runs), and the default path is untouched. Two stated global constraints are violated:

- The cache key has no engine/VM identity, so a rebuilt VM silently reuses stale signals.
- Pinned weights carry no TRAIN binding, so validation-only runs can iterate over weights.
