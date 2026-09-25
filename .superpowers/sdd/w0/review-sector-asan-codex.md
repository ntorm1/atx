# W0-R04 native ASan gate review

## Verdict

APPROVE for integration. This closes the scoped missing-sector bounds sanitizer check;
it does not qualify the whole engine under ASan or UBSan.

## Reviewed SHA

370f7af4ad3fadf1551ca045c82e85539461b0d1, independently reviewed by root after implementation.
The four changed files are the preset, guarded test-target wiring, existing sector fixture,
and scoped runner. No production numeric source is changed.

## Evidence

Read the complete diff, compile configuration, runner, and positive-control stderr.
The target compiles actual production factor_model.cpp and the fixture with address
instrumentation, Release CRT, /Od, NDEBUG, /WX and PCH off. The fixture rejects a build
without the compiler's address-sanitizer feature. Coherent MSVC runtime/import/thunk paths
are detected from vcvars; missing components or invalid configurations fail closed.

Independently executed the binary from root's pool, writing only the root-owned log:

```
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=0
ATX_ASAN_FORCE_PRE_W0_OOB unset; symbolizer from the configured cache
C:/atx-wt/pool-4/build-equity-asan/bin/atx-engine-risk-sector-asan.exe --gtest_filter=RiskSectorColumnsById.* --gtest_brief=1
[==========] 5 tests from 1 test suite ran. (199 ms total)
[  PASSED  ] 5 tests.
exit=0
SHA256=4B4F892532390B2794B184911E8930161E2F926B51C9B39B62117B23D5B45533
```

Receipt: pool-2/build-equity/w0-asan-independent.log. The independent run includes the
death test requiring the native ASan diagnostic. The owner's separately captured forced
control also shows READ of size8 exactly zero bytes after a16-byte region at the original
beta[2] read (fixture line199), with AddressSanitizer: heap-buffer-overflow and exit1.
Thus Eigen assertions cannot substitute for instrumentation in this NDEBUG gate.

## Findings and limits

No blocker. Ordinary Debug expectations are retained. The old UB is deliberately confined
to the existing death-test/forced-negative-control path. The runner restores its sanitizer
and control environment, checks both exits, and requires the diagnostic rather than any crash.
Its build is target-scoped and its dependencies are isolated from other presets.

Support libraries and vcpkg are uninstrumented. Target-private STL string/vector logical-size
annotations are disabled to match their ABI; size-within-capacity checks are outside this
gate. Raw allocation bounds are still instrumented and independently proven. No UBSan,
leak-sanitizer, whole-engine, or general absence-of-UB claim follows from these results.

The earlier bundled LLVM runtime startup failure is not a pass: the final coherent MSVC
runtime set replaced it and completed both the detection control and corrected fixtures.

## Checked

- C++ checklist applied; the owner's implementation-before-tests instruction governs.
- Diff stays within the explicitly expanded W0-O1/R04 gate scope.
- Evidence independently reproduced; no acceptance threshold or warning was weakened.
- Source, fixture and dependencies have an explicit instrumentation boundary.
