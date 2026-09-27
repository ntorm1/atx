# Task T7 review: IC runner pinned PIT fields, plus T1 fix round 1

Package: `review-T7.diff` (060f440c, a9ef7175, ac0d1309, e238c94c, ccd05314). Source line numbers are from
`C:/atx-wt/pool-2` HEAD 768e3eb8. The T7-owned files there are unchanged since ccd05314. Build and test evidence
is root's (54/54 after fix round 1); I did not re-run it.

### Spec Compliance

- ✅ **Spec compliant.** Every brief requirement and every root addition (a)–(d) is implemented. One deviation from
  scope was accepted by root (M6).
  - **R1 CLI.** Options are parsed at `strategy_ic_runner.cpp:1445-1448`. Pairing, plus
    `--validation-fields` requiring `--validation`, is enforced at `:1288-1290`. The default path is unchanged:
    - the recipe key set is asserted exactly (`test:1029-1068`);
    - with fields pinned but unreferenced, the numeric outputs are byte-identical.
  - **R2 binding before any payload, including `--plan-only`.** `bind_fields` (`:525-583`) is called right after
    each `admit` (`:1301`, `:1305`), which is before the plan-only branch. In order it checks:
    - the hash pin;
    - schema and `status`;
    - `role.manifest_sha256`;
    - the `sessions`/`ids` receipts, `dates` and `instruments` (`:538-548`);
    - per-entry file name, dtype, layout, shape, SHA and bytes (`:554-563`).
  - **R3 referenced-only loading.**
    - Referenced fields come from the compiled `Program::fields` (`:440-446`, `:452`).
    - An undeclared field refuses at `:438-439`. A declared field that is missing from the manifest refuses at
      `:564-569`.
    - For a scored role, extents are stat'ed (`:579-582`) and the payload is streamed through the SHA check
      (`load_fields` `:925-935`).
  - **R4 DSL panel.**
    - Built with `Panel::create_borrowed`: no column is copied, and a 1 B/cell presence mask comes from
      `in_universe` (`:940-954`).
    - `set_cross_section_mask(role.decision_member)` is unchanged.
    - With no extras, the VM gets `role.panel` itself (`:1098`).
  - **R5 memory admission.** `cells × (8k+1)` is added only when k > 0 (`:481-482`).
  - **R6 cache key.**
    - Field candidates are stored under `ROOT/<fields-sha>/`; base candidates keep `ROOT/<role-sha>/`
      (`:689-694`).
    - The sidecar must match: base entries must not name a manifest, and field entries must name ours
      (`:749-751`).
  - **R7 records.**
    - recipe: `:1348`
    - summary: `:1366`
    - role result: `:1263-1272`
    - saved combined manifest: `:389`
  - **(a) declared set relaxed** to {close, raw_close, volume} ∪ the fields in the manifest (`:405-414`,
    `:564-569`).
  - **(b) non-PIT or missing flag refuses.** Missing or contradictory flags refuse the whole manifest (`:506-519`).
    A declared field that is not point-in-time refuses (`:571-575`). There is no override.
  - **(c) T1 C1: VM identity in the key and sidecar** (`:54-99`, `:685-687`, `:697-703`, `:752`).
    - It is a clean miss across identities and a loud refusal for a foreign sidecar.
    - The residual risk is I2.
  - **(c) T1 I1: TRAIN binding.**
    - `train_manifest_sha256` must equal `--train-sha256` (`:664-668`).
    - A weighted frozen source requires exactly the same weights pin (`:253-256`).
  - **(d) signs.**
    - Values must be integer ±1 for known ids, and every candidate with weight > 0 must have one (`:610-633`).
    - They are applied over the IC orientation in the blend only (`:1181-1183`).
    - This matches the T11 emitter: `task-T11-report.md:51-60` gives absolute raw-signal signs, which are
      compared with `runner_sign`.
  - **Fixtures (a)–(e):**
    - (a) `test:860`
    - (b) `test:915`
    - (c) `test:953`
    - (d) `test:1029`
    - (e) `test:1070`
  - **Plus:** PIT `test:1196`, frozen resume `test:1162`, signs `test:1240`, identity `test:1285`, workers
    `test:1327`, and TRAIN-bound resume `test:742`.
- ⚠️ **Cannot verify from diff:**
  1. **v3 real-TRAIN memory against the 1536 MiB ruling.** This is an estimate; see I1. Root should run native
     `--plan-only` for v3 on the (repaired) TRAIN role, at the intended `--workers`, before scheduling the run.
  2. **The T11 fitter cannot find v3's field candidates.**
     - `atx-impl/tools/fit_composition_weights.py:860` reads only `DIR/<train_sha>/`.
     - Its sidecar check (`:248-264`) ignores `vm_identity` and `fields_manifest_sha256`.
     - v3 field candidates live in `DIR/<fields-sha>/`, so the fitter fails loudly with "missing entry".
     - A non-legacy build (e.g. rel-avx2) writes under `DIR/<identity>/`, while the fitter would still read
       dev-build entries.
     - The T11 lane must adopt the rule in `task-T7-report.md` §"Cache path rule" before the v3 fit.
  3. **The legacy keyless-sidecar exemption is valid only at today's HEAD.** I verified it. It stays valid only
     until the next alpha, parallel, core or data change; any such change must bump `dsl_vm_semantics_version`.
  4. **The real fields directories must be regenerated** with the c099cade producer against the repaired roles.
     The runner refuses the old `*-fields-v1` directories because they have no PIT flags (decision 7).

### Checks run (named risk, then what I checked)

- **Could the legacy exemption serve pre-change bits?** No, not at current HEAD.
  - `git diff --stat 429cbe43 HEAD` and `6d85ac2a HEAD` cover `atx-engine/{include,src}/…/alpha`, `parallel`,
    `atx-core/{include,src}` and `atx-engine/{src,include}/…/data`.
  - The only difference is the `vm.hpp` comment (ac0d1309).
- **Could base candidates evaluated on the borrowed DSL panel write different bytes into the shared base key?**
  No.
  - `Panel::create_borrowed` (`atx-engine/src/alpha/panel.cpp:43-70`) stores the same spans and names, plus the
    copied universe.
  - The VM resolves fields by name (`vm.hpp:1131`) and masks with `in_universe` (`vm.hpp:1098`, `:1310`).
  - The base names keep their order, and there are only three of them (`strategy_data.cpp:157`), so no name
    can collide with an extra (`:554`).
  - This is untested, though; see M3.
- **Do the VM kernels live in separately compiled TUs, so the runner-TU flavor macros might misdescribe them?**
  - `atx-engine/src/alpha/` holds no VM, cs or ts kernel `.cpp`; they are header-only, instantiated here.
  - The parser, typecheck and bytecode TUs are FP-insensitive.
  - The presets are global. This holds under today's build (see I2 note).
- **Could replay refuse the new manifest keys?**
  - `strategy_target_replay.cpp:372-395` checks named keys only; unknown keys are tolerated.
  - `signal_semantics` is unchanged when signs are pinned (`:385-387` diff context), so replay still admits
    the blend.
- **Does the pinned-sign meaning match the fitter?** Yes.
  - T11 emits absolute s_k, compared with `runner_sign` (`task-T11-report.md:51-60`), so replacing the
    orientation is correct.
  - Multiplying the two would have been wrong.
- **Lifetime of the borrowed panel and extras on early returns.** Declaration order is extras (`:1079`), then
  role, then `field_panel`, then `vm`. Reverse destruction therefore destroys the Engine first. Explicit
  release happens at `:1214-1215`.

### Strengths

- **The DSL panel design is minimal and exact.**
  - Zero column copies, and 1 B/cell of owned state.
  - The default path hands the VM `role.panel` itself, so "absent option ⇒ identical" holds by construction,
    and test (d) asserts it.
- **Every binding check runs before any payload, in plan-only and in real mode.**
  - The refusal fixtures delete the role's `close.f64`, so any premature payload read would show up as a
    different error (`test:959-960`, `:1203`).
  - A same-size bit flip passes plan-only, then refuses on the streamed hash before the role is opened
    (`test:1017-1027`).
- **The fields cache key cannot cross-contaminate.** Field entries sit in their own manifest-keyed directory
  with a sidecar match. A base lookup refuses any sidecar that names a manifest. Test (e) shows old entries and
  base sidecar bytes left untouched, and a transplanted entry refused.
- **The PIT guard has no override.** It refuses unflagged or contradictory manifests outright, and non-PIT
  entries are harmless while undeclared (`test:1196-1238`).
- **T1 fix round 1 is careful.**
  - Weighted frozen sources are pinned exactly.
  - Signs are strict JSON integers; `1.0` and `"1"` are refused.
  - IC diagnostics keep the TRAIN orientation, and `composition_weight`/`composition_sign` are recorded for NR
    (`:1192-1193`).
  - Every refusal is tested in plan-only and real mode.
- **Fix round 1 root cause is correct.** A C++20 range-for over `read_json(...).at(...)` binds into a destroyed
  temporary: no lifetime extension applies through a member call; P2718 fixes it only in C++23. All four sites
  are fixed with named locals.

### Issues

#### Critical (Must Fix)

None.

#### Important (Should Fix)

**I1. All referenced extras stay resident for the whole candidate loop, which likely puts library v3's real
TRAIN run over the 1536 MiB ruling.**
Code: `strategy_ic_runner.cpp:481-482` (admission), `:1079` (load all), `:1214-1215` (release after the loop).

- **The arithmetic** (an estimate; confirm per ⚠️1):
  - The TRAIN role has 1155 × 5627 = 6,499,185 cells.
  - `progress.md:198-199` gives v2's plan-only TRAIN admission as 1,234,268,377 B at 8 slots. By `admit`'s
    formula (`:475-497`) that figure is exactly workers=4: composition 58.77 MB + worker envelope 58.60 MB.
  - `pv_fields_ic121_v3.json` declares 8 extras, and all 8 are referenced. `admit` therefore adds
    65 B/cell = 422,447,025 B.
  - v3 then needs about 1,656.7 MB ≈ **1580 MiB at workers=4**. That exceeds the 1536 MiB ruling
    (`progress.md:63`), so admission refuses.
  - At workers=1 it is about 1,598.1 MB ≈ **1524 MiB**. That leaves about 12 MiB of RSS headroom for everything
    admission does not count (binary, CRT heap, JSON), and `:491-492` itself says admission "is not a measured
    RSS guarantee".
  - Any extra VM slot adds 52 MB.
- **Why it matters.** The global constraint pairs "only referenced fields are loaded" with the RAM cap. Loading
  only referenced fields is necessary but not sufficient here. v3 is the library the deliverable depends on.
  Splitting the library instead would split the orientations artifact that T11 pins.
- **Fix (inside T7's files).** Make extras resident per candidate.
  - Load a field when the first candidate that references it is reached, in library order.
  - Release it after the last candidate that references it.
  - Rebuild the borrowed panel and Engine only when the resident set changes.
  - Admit the peak concurrent set, not the union.
  - I checked v3 read-only (a regex over the DSL):
    - all 8 extras are referenced;
    - each candidate uses at most 2 (70 use none, 38 use 1, 13 use 2);
    - with first-use/last-use lifetimes in library order, the peak concurrent set is 4 fields.
  - The peak therefore drops from 65 to 33 B/cell, about 208 MB less, which puts v3 near 1380 MiB at
    workers=4.
- **At minimum,** run v3 `--plan-only` now and record `required_bytes` in the ledger.

**I2. T1 C1 is only partly closed: the VM identity is a hand-bumped constant with no tripwire.**
Code: `strategy_ic_runner.cpp:54-60`; `vm.hpp:12-15`.

- **What:** `dsl_vm_semantics_version` changes only if an editor remembers to bump it.
  - The pointer comment exists only in `vm.hpp`.
  - The other sources the runner comment lists carry no pointer: cs/ts/state ops, parser, typecheck, dag,
    bytecode and fusion.
  - A kernel fix without a bump silently serves pre-fix signals from `build-equity/mega-candidate-cache`, to
    both the runner and the T11 fitter. The implementer acknowledges this (`task-T1-report.md` concern 3).
- **Why it matters:** the global constraint is "the cache must never serve a stale signal". The cache
  outlives this sprint, and the vec_avg member-mask question from T5 is exactly the kind of kernel change
  that would be missed.
  - The flavor macros also describe only this TU. That is fine under today's global presets, but it becomes
    wrong if a per-target FP flag is ever introduced.
- **Fix (cheap, owned test file):**
  - Add a GoogleTest that SHA-256s the VM-relevant sources (`atx-engine/include/atx/engine/alpha/*` plus
    `atx-engine/src/alpha/*.cpp`, reachable from `ATX_IMPL_TESTS_DIR`).
  - Compare the digest against a constant stored beside `dsl_vm_semantics_version`.
  - On a mismatch, fail with "re-pin the digest and decide whether to bump `dsl_vm_semantics_version`".
  - This keeps the implementer's point (comment-only edits need not invalidate the cache) while removing the
    silent path.

#### Minor (Nice to Have)

- **M1. A residual path for picking weights on validation remains** (root-ruled contract).
  - Code: `:253-256`, `:664-668`.
  - An unweighted frozen source admits any weights file that self-declares the right `train_manifest_sha256`.
  - Repeated validation-only runs with different fits are therefore possible. They are auditable only through
    the recipe's weights SHA.
  - T11 already carries the orientations SHA in `provenance`. In validation-only mode, requiring it to equal
    `--orientations-sha256` would tie the weights to the frozen TRAIN run.
- **M2. Presence masking of extras is untested.**
  - The fixture's `present.u8` is all ones (`test:52`).
  - No test shows an extra reading NaN where `present`=0 but the file value is finite.
  - That behaviour is what `dsl_panel` exists for (`:951-952`).
- **M3. Base candidates evaluated in a fields run publish into the shared `ROOT/<role-sha>/`, but no test
  compares their bytes.**
  - Code: `:689-694`.
  - A fields run writes those entries without asserting they equal a base-only run's. `test:886` checks only
    that the fields key is absent.
  - I verified by reading that they are equal (Checks). Add a `file_sha` equality to test (a) against a plain
    cached run.
- **M4. A stale header comment.** `strategy_ic_runner.hpp:18-19` still says
  `DIR/<role-manifest-sha256>/`. The layout is now `DIR[/<vm-identity>]/<role-or-fields-sha>/`.
- **M5. Field definitions are not checked across roles.**
  - `bind_fields` (`:525-583`) checks only name, PIT, shape and hash.
  - A validation manifest from a different producer version would pass if it uses the same names but different
    `sources`, `clock` or `units` (e.g. a changed `mkt_ret` definition).
  - Both SHAs are recorded, so this is auditable. A cheap improvement: when both manifests are pinned,
    compare `sources`, `clock` and `units` per declared field.
- **M6. A public engine header was touched although the brief forbids it.**
  - `vm.hpp:12-15` (ac0d1309). The T7 and T1 briefs both say "do not change public engine headers".
  - It is comment-only, disclosed, and root imported it knowingly (`progress.md:100-101`). It cost about 52
    TUs of rebuild.
- **M7. The report overstates the lookup.** It says lookup requires `research_fields`, but `cached_payload_sha`
  matches only `fields_manifest_sha256` (`:749-751`). This is harmless because `dsl_sha256` fixes the fields.
- **M8. T1 minors remain open:**
  - M2: fail-fast sidecar pre-scan;
  - M3: shared hex helper;
  - M5: `ASSERT_DOUBLE_EQ` where exact;
  - M6: untested `cache_preflight`, corrupt-sidecar and unit-level composition refusals.

  These were deferred explicitly (T1 report, "Other review items"), and I carry them forward here.

### Assessment

**Task quality:** Needs fixes

**Reasoning:**
- The fields path is correct and carefully bound: borrowed panel, metadata binding before any payload, a
  manifest-keyed cache with sidecar match, and a PIT guard with no override. T1 fix round 1 closes the
  TRAIN-binding and signs contract, with thorough refusal tests.
- Two things keep it from approval:
  - With all referenced extras resident for the whole loop, the v3 real TRAIN run is at or over the
    1536 MiB cap by arithmetic from recorded plan numbers.
  - The VM-identity fix still relies on a manual version bump, with no tripwire against a "never stale"
    constraint.
