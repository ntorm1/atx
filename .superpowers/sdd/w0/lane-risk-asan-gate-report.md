# W0-O1 extension: native W0-R04 bounds sanitizer gate

Status: **DONE for the scoped R04 memory-bounds acceptance**. No UBSan, leak,
whole-engine, or STL logical-size-within-capacity qualification is claimed.

Code commit: `370f7af4ad3fadf1551ca045c82e85539461b0d1` on
`feat/w0-replay-integration-codex-20260925`, worktree `C:/atx-wt/pool-4`.
The compiled sanitizer source/configuration inputs exactly match that commit.
Unrelated subsequent baseline/book summary edits do not enter this target.

## Why this gate was added

The pre-W0 sector-copy loop read `beta[c]` using the latest cross-section's column
count even when an older cross-section had fewer columns. This is a concrete
out-of-bounds read. `.agents/cpp/agent.md` section 8 explicitly reverses its ASan
decline when a sanitizer-detectable defect lands. Root authorized this bounded
W0-O1 extension under that reversal. Checked indexing alone was insufficient for
the R04 sanitizer acceptance. Implementation preceded the added sanitizer control;
the existing economic/numerical fixture assertions were preserved.

## Implementation and qualification boundary

`equity-asan` inherits the static equity shape, disables PCH and IPO, uses the
Release CRT (`/MD`) with `/Od /Ob0 /DNDEBUG`, and isolates FetchContent to
`${sourceDir}/deps/equity-asan`. Its build preset targets only
`atx-engine-risk-sector-asan`. The target compiles the actual production
`atx-engine/src/risk/factor_model.cpp` and the existing sector-column fixture with
`/fsanitize=address /Oy- /Z7`, `/W4 /WX`, and the source-side inline helpers they
include. It does not link the whole `atx::engine` target.

Compiled dependencies (`atx::core`, test process support, GoogleTest, and vcpkg/
third-party libraries) are **uninstrumented**. Instrumented accesses in the owned
translation units are checked, including the Eigen allocation bounds at issue.
Target-private `_DISABLE_STRING_ANNOTATION` and `_DISABLE_VECTOR_ANNOTATION` match
the prebuilt dependencies' MSVC-STL ABI. Accordingly, this gate does **not** check
string/vector logical-size violations that stay inside their allocated capacity.
No global compiler flags or shared vcpkg files were changed. No production source
was changed to silence instrumentation.

The native compiler is Visual Studio's clang-cl 18.1.8. Runtime provider `msvc`
detects the active toolset via `VCToolsInstallDir` supplied by the build wrapper,
then uses its matching import library, runtime thunk, and DLL. Every required
component and llvm-symbolizer is checked during configure; absence fails loudly.
This host used MSVC 14.42.34433 and its
`bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll`, SHA-256
`B87E517E64B369D70053B2470546722FCBA15AFD46324BFB5984E5388B71D98E`.

## Reproduction and observed results

From the owning worktree:

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL = '1'
& C:\atx-wt\pool-4\scripts\test-risk-sector-asan.ps1 -Jobs 1
```

The runner uses `scripts/atx-build.ps1` for configure and the scoped target build,
checks the exact per-worktree dependency directory, requires more than 4 GiB free
RAM before compiling, and records source revision/dirty status and executable
SHA-256. The successful attempt observed 5.5368 GiB free. No real market data was
read; every fixture is synthetic.

The runner starts the exact historical access in a separate intentional-failure
process. It requires both a nonzero exit and the native ASan diagnostic. NDEBUG
disables Eigen's debug assertions, and a compile-time feature check rejects an
uninstrumented control fixture.

Successful native positive-control receipt (process exit **1**):

```text
==16868==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x12837f2a1a60 at pc 0x7ff7c7df1f71 bp 0x00e48db5d8a0 sp 0x00e48db5d8a8
READ of size 8 at 0x12837f2a1a60 thread T0
    #0 0x7ff7c7df1f70 in atx_test_w0_r0_sector_columns::RiskSectorColumnsById_CheckedIndexingCatchesThePreW0Read_Test::TestBody::<lambda_0>::operator() C:\atx-wt\pool-4\build-equity-asan\atx-engine\tests\risk\risk_w0r0_sector_columns_test.cpp:199
0x12837f2a1a60 is located 0 bytes after 16-byte region [0x12837f2a1a50,0x12837f2a1a60)
```

The allocation stack identifies `Eigen::internal::aligned_malloc` and the
two-element beta vector. Source paths in the stack reflect the build's configured
prefix mapping; the source line is the preserved `fit->beta[c]` read.

The subsequent instrumented suite passed **5/5 tests**, **0 skipped**, **325 ms**,
exit **0**, with no unexpected sanitizer diagnostic. It covers the corrected
production builder on dates with missing groups, disappearing groups, changing
point-in-time membership, and identity mapping, plus the expected failing control.
The runner itself returned **0**.

Executable SHA-256:
`4B4F892532390B2794B184911E8930161E2F926B51C9B39B62117B23D5B45533`.
Root independently ran that same binary: **5/5**, **199 ms**, exit **0**, and
independently verified the native read-size/allocation-boundary control diagnostic.
Root's receipt is `build-equity/w0-asan-independent.log` in its own worktree.

Local logs:

- `build-equity/w0-risk-asan-msvc.log`: successful wrapper build and runner.
- `build-equity-asan/sector-asan-positive.stderr.log`: full native control stack.
- `build-equity-asan/sector-asan-suite.stdout.log`: five passing fixtures.
- `build-equity-asan/sector-asan-suite.stderr.log`: empty on the successful suite.

## Failures resolved while constructing the gate

The first attempt failed `/WX` on deprecated `getenv`; the control now uses
Windows `getenv_s`. The next link rejected mismatched MSVC-STL annotations;
target-private compatibility definitions resolve that with the limitation above.
LLVM 18's bundled Windows runtime then failed startup (access violation after a
failed `strdup` interception). Merely substituting the MSVC DLL was invalid because
its exports differ from the LLVM import/thunk set. The final gate instead links and
deploys the coherent MSVC runtime set, which passed both controls. The explicit
`clang` runtime provider remains available for diagnostics but was not qualified
on this host. `/MDd` is rejected by this clang sanitizer, so Release CRT plus `/Od`
is intentional; this is not a Debug-CRT or optimization-performance qualification.
