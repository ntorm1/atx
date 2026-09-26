# W0 compiler and incremental-build correction

Status: wrapper/caller checks and configured-graph checks passed. Scoped PCH compilation, post-build no-op proof, and fresh hygiene configuration remain pending the shared-host memory/slot gate. Do not treat the static evidence as a completed compiler gate or a measured speedup.

## Scope and commits

- `440116f360b5edd972c0f2d75bdb1b59a9f6cdec`: bounded build/check workers, separate equity-hygiene preset, source-local discovery provenance, owning wrapper checks/docs.
- `464d04836bfc6eb0cb76a76ecb3dac3ff328de24`: reject every forwarded `-j` prefix, including Ninja's accepted signed/equals forms.
- `0c5f87a0daa67f11f1952fc76be81a954ba9c7f7`: migrate seven oracle preparation callers to `-Jobs 2`, update four existing assertions, add isolated caller-to-wrapper dry-run checks.
- `3f2c25fbdf608fb08270316e424333d08d4a0ab1`: two minimal stable engine-test PCH carriers. Kept separate so integration can retain warm W0 test objects while this change completes its scoped compiler qualification.

All work is in pool-4 on `feat/w0-replay-integration-codex-20260925`. Warning, FP, ISA, optimization, CRT and benchmark production settings are unchanged. No benchmark, market-data, database or oracle workflow was run by this audit. Graph tools were unavailable; discovery used targeted source/config searches.

## Findings and corrections

1. `atx-impl/CMakeLists.txt` attached `ATX_ENGINE_GIT_SHA` to all 34 implementation-core TUs. Its sole production consumer is `src/stage_discover.cpp:821`. A configure following any commit changed all those commands. The local prior disclosure rebuild log has 43 steps including all 34 core TUs despite only two implementation sources changing. The definition now belongs only to `stage_discover.cpp`; its persisted provenance remains intact. The first transition still invalidates commands that lose the old definition.
2. Engine tests selected the first enabled test executable as PCH owner. Group selection/order changed paths and made other consumers depend on that executable's full build. `atx-engine-test-pch` is now an OBJECT carrier containing only tracked `tests/pch.cpp`; data uses `atx-engine-data-test-pch` because its miniz PUBLIC include directory makes the previous uniform-environment comment false. Carriers are created only for enabled consumers with PCH enabled. Executable scratch isolation stays on the actual test executables. Carriers have no worker or test executable dependencies.
3. `check` invoked Ninja directly, bypassing `CMAKE_BUILD_PARALLEL_LEVEL`; build previously relied on forwarded raw flags. Both now select explicit `-Jobs`, else nonempty valid `CMAKE_BUILD_PARALLEL_LEVEL`, else one worker. Valid limits are integers 1..256. Explicit Jobs overrides even malformed environment text; malformed/zero environment values without it fail. Ctest retains its own explicit/default Jobs behavior. Forwarded native concurrency flags fail clearly, so later `--parallel`, `--jobs` or `-j...` cannot override the selected cap. Bare all-target builds remain rejected.
4. `equity-hygiene` inherits equity-dev's Debug/compiler/CRT/warnings, sets PCH OFF, and uses `build-equity-hygiene` plus `deps/equity-hygiene`. Future header checks must use it instead of flipping equity-dev OFF then ON and invalidating warm consumers.

## Measured evidence

The orchestrator measured its completed integrated serial build at 394 outputs / 2375.269s: 222 engine-test objects took 1367s, 93 implementation-test objects 542s, 67 production/dependency outputs 404s, and 12 links 61.5s. These are its build measurements, not a controlled before/after speed comparison for this patch. Pool-4 historical Ninja entries put PCH generation at approximately 10.775s (factory owner), 9.613s (data owner), 3.820s (book owner), and 3.419s (engine).

Pool-4 configured `equity-dev` with `book;data`, then `data;book`, preserving `FETCHCONTENT_BASE_DIR=C:/atx-wt/pool-4/deps/equity-dev`, PCH ON, and disconnected FetchContent updates. Both metadata-only configures succeeded; vcpkg reported all dependencies installed and no package or C++ build was launched. Configure/generate took approximately 35.7s and 31.1s, respectively.

- 344 existing compile commands match after removing only expected source/output/PCH-path/provenance differences. No warning, optimization, CRT, ISA, FP, include-order or other definition changes were observed.
- The only configured Git-SHA consumer is `stage_discover.cpp`.
- A subsequent metadata-only configure after the commit changed compared all 385 complete command strings: only the old/new `stage_discover.cpp` SHA definition differs; stripping that definition leaves zero differences. Configure/generate took 13.8s/0.3s. This proves command-local invalidation; it is not a timed rebuild claim.
- All 18 configured book test TUs match the common carrier's normalized compilation environment; all 37 data test TUs match the data carrier's environment.
- `ninja -t commands atx-engine-test-pch` and the corresponding data command each report exactly two commands: its PCH and empty carrier TU. There are zero test-source or worker commands in either closure.
- Three generated PCH source/header files retained timestamp `1790375503386` Unix milliseconds across the group reorder (actual NTFS UTC `2026-09-25T22:31:43.3864511Z`). A first exact-DateTime comparison falsely differed because Windows PowerShell JSON rounded submillisecond ticks; the receipt records both rounded timestamps and current hashes.
- `scripts/atx-build-dryrun-test.ps1 -CheckPreset equity-dev` passed after implementation. It covers bounded build/check argv, precedence, malformed/zero limits, Ctest independence, refusal of raw concurrency flags (including `-j+4` and `-j=4`), and actual two-source check resolution. Installed Ninja accepts both attached forms, so the broad guard is necessary. The auditor independently passed this same suite.
- `scripts/tests/oracle-targeted-build-args.Tests.ps1`: 7/7 passed, 23.01s. Each pure GetSpec call with fixture identity sends its exact preparation argv through the real wrapper's DryRun and verifies preset directory, targets and two-worker result. No oracle executable, cohort, store or workflow is invoked. The larger existing oracle suite cannot load in this equity checkout because its retired atx-vol source fixtures are absent; its four changed assertions were updated but are not claimed executed.

Local receipts under `build-equity/` (ignored build artifacts): `w0-build-wrapper-tests-final.log`, `w0-oracle-build-args-tests.log`, `w0-build-audit-configure.log`, `w0-build-audit-reconfigure.log`, `w0-build-commands-before.json`, `w0-command-audit.ps1`, `w0-build-command-audit.json`, `w0-pch-generated-before.json`, `w0-pch-reorder-stability.json`, `w0-pch-closure.json`, `w0-localized-command-before.json`, `w0-localized-command-proof.json`, and `w0-localized-command-configure.log`. The command-audit helper preserves the normalization procedure; closure receipts retain complete commands.

## Cache diagnosis and limits

The host uses ccache 4.13.6 and clang-cl 18.1.8 with `/Z7`; required PCH sloppiness is already configured. The initial cumulative cache snapshot was 18787 calls, 11124 cacheable, 5754 hits, 5370 misses, and 7663 uncacheable (7581 preprocessing failures, 82 compile failures). These historical counters do not identify this build's cause. No cache cleanup, reset, speculative compiler flag or global cache change was made.

The benchmark owner subsequently reported per-build telemetry: 224 calls, 84 direct hits, 140 misses, zero preprocessing failures. Separate read-only diagnosis owns the fresh logs. The older cumulative failures must not be attributed to the current compiler configuration. In particular, absent `-Xclang -fno-pch-timestamp` alone does not establish the cause, so this patch does not alter it.

An initial Ninja dry-run stopped at its pending CMake regeneration; that is not evidence of a no-op target. Remaining qualification is deliberately narrow: compile one existing book and one existing data consumer with their carriers through the wrapper at one worker, then rerun those exact object checks for a no-work result; configure isolated equity-hygiene and confirm no PCH owner/flags. Inspect the planned closure before launching. Do not rebuild the full suite solely for this CMake change. These checks wait for the orchestrator's slot and memory floor; no acceptance is waived.
