# Build-System Worktree-Speed Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a fresh git worktree's first build of atx (and especially atx-vol) take minutes instead of the better part of an hour, by replacing the broken sccache-based cross-worktree cache with an empirically verified ccache configuration, deduplicating the per-worktree vcpkg payload, and unifying the fragmented preset/script surface.

**Architecture:** Compiler-level object caching (ccache with `base_dir`/`hash_dir=false`/`ignore_options`) makes first-party objects transfer byte-identically across worktrees; a shared `VCPKG_INSTALLED_DIR` and the existing shared `FETCHCONTENT_BASE_DIR` remove the per-worktree dependency tax; PCH stays on for inner-loop speed (ccache caches PCH-using TUs within a worktree — sccache could not cache them at all). Test discovery moves off the build critical path.

**Tech Stack:** CMake ≥3.29 presets, Ninja, clang-cl (VS 2022 LLVM), ccache 4.13.6, vcpkg manifest mode, GoogleTest/CTest.

## Diagnosis (evidence, 2026-07-12)

Every mechanism below was verified on this machine; do not re-litigate these findings, but re-verify the marked ⚠ items during implementation.

1. **PCH made sccache useless.** `sccache --show-stats`: 450 of 1145 compile requests non-cacheable, reasons `/Fp` (447) and `/Yc` (3). Every TU of every PCH-enabled target (atx-engine, atx-engine tests, atx-vol-tests) bypassed the cache entirely. The repo's two headline speed mechanisms (PCH + sccache) were mutually exclusive per-TU.
2. **`SCCACHE_BASEDIR` is a no-op.** Verified: restarting the sccache 0.15.0 server with `SCCACHE_BASEDIR`/`SCCACHE_BASE_DIR` set still shows `Base directories (none)` in stats. The cross-worktree design described in `scripts/dev-setup.md` ("the key unlock") never functioned. Cross-worktree first-party hits: effectively zero (observed hit rate 31.6%, all same-path rebuilds).
3. **ccache 4.13.6 + clang-cl works, with exactly three config keys** (verified with the repo's exact flag set `/nologo -TP /EHsc /Od -MDd -Z7 -std:c++20 /W4 /WX /clang:-ffile-prefix-map=<wt>=.`):
   - `CCACHE_BASEDIR=<worktree root>` — relativizes worktree-internal absolute paths in the hash;
   - `CCACHE_NOHASHDIR=1` — stops hashing the build cwd (safe because `-ffile-prefix-map` already scrubs cwd from the object);
   - `CCACHE_IGNOREOPTIONS=-ffile-prefix-map=* /clang:-ffile-prefix-map=*` — the per-worktree prefix-map argument value otherwise lands in the hash (ccache's built-in prefix-map exclusion does not fire for the `/clang:`-wrapped spelling).
   Result: compile in worktree A, recompile identical source in worktree B → **cache hit**, and the two objects are **byte-identical** (so serving A's object to B is exactly correct).
4. **MSVC-style PCH (`/Yc /Yu /Fp`) IS cacheable by ccache** (with `CCACHE_SLOPPINESS=pch_defines,time_macros`): same-worktree recompiles hit 100%. Cross-worktree PCH-consumer compiles miss structurally — the `.pch` files differ at byte level across worktrees (clang PCH serialization embeds absolute paths; `-ffile-prefix-map` does not reach them). Accepted: a fresh worktree recompiles PCH-consuming TUs once at PCH speed (~3-5s/TU instead of 10-30s), then hits its own cache.
5. **1.1 GB `vcpkg_installed` duplicated into every worktree's build dir** at configure time (plus install wall time). `build/` totals 5 GB per worktree (static libs).
6. **Preset/script fragmentation footgun:** `scripts/new-worktree.ps1` configures preset `dev` (shared FetchContent deps at `C:\atx-cache\deps`), but `scripts/atx-build.ps1 configure` reconfigures the **same** `build/` dir as preset `ninja` — which does NOT set `FETCHCONTENT_BASE_DIR`, so deps re-clone/re-build into `build/_deps` and the configure churns. Agents flip between them mid-sprint without noticing.
7. **Test discovery on the build path:** every `gtest_discover_tests()` call (atx-vol has two against one binary; engine/core/tsdb/impl one each) runs the freshly linked exe at build time (default `POST_BUILD` mode) to enumerate ~2600 tests. It also breaks the build when the exe can't launch (observed 2026-07-12, "failure in test discovery phase").
8. **Ninja-log timings** (Release, full build): ~4.9 CPU-hours total. atx-vol lib 2525s + atx-vol tests 1077s CPU. Worst TUs: Eigen-heavy calibrators 45-58s each; unity test batches 130-220s. Header parse dominates (sccache measured mean compile 32.5s). This plan does NOT restructure headers (follow-up work; see Out of Scope).

## Global Constraints

- Windows 11, VS 2022 Community clang-cl, CMake 3.29.0, Ninja from the VS bundle (`atx-build.ps1` provides it). All scripts must stay Windows PowerShell 5.1-safe, ASCII-only.
- `/W4 /WX` (`ATX_WERROR=ON`) stays on for all first-party targets; nothing in this plan may weaken warnings.
- The `hygiene` preset must keep `ATX_USE_PCH=OFF` and remain the include-hygiene gate.
- Machine-local shared caches live under `C:\atx-cache\` (already exists: `bin/`, `deps/`). ccache binary already installed at `C:\atx-cache\bin\ccache.exe` (4.13.6), which is on PATH.
- Do NOT touch the user's uncommitted working-tree changes (`.vscode/settings.json`, `CMakeLists.txt` line `add_subdirectory(atx-ui)`, `atx-vol/CMakeLists.txt` universe_autofit block). Build on top of them; never revert.
- Do NOT run a cold full build to "measure the before" — the user has stated it is too slow; the ninja-log numbers above are the baseline.
- The default `ninja` preset and CI path must keep working if ccache is absent (`find_program` fallback, like today's sccache wiring).

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `CMakeLists.txt` (root) | modify | compiler-cache launcher block: prefer ccache, keep sccache fallback; shared-FetchContent fallback logic |
| `CMakePresets.json` | modify | ccache env keys in `_base`; shared `VCPKG_INSTALLED_DIR`; retire the `dev`/`ninja` split (make `_base` carry shared deps); keep `hygiene`/`rel`/`rel-avx2`/`vs` |
| `atx-vol/tests/CMakeLists.txt` | modify | `DISCOVERY_MODE PRE_TEST` on both discover calls |
| `atx-engine/tests/CMakeLists.txt`, `atx-core/tests/CMakeLists.txt`, `atx-tsdb/tests/CMakeLists.txt`, `atx-impl/tests/CMakeLists.txt` | modify | same discovery-mode change |
| `scripts/dev-setup.ps1` | modify | install/point-at ccache, set its global config (cache dir, size, sloppiness), keep sccache lines for rollback note |
| `scripts/atx-build.ps1` | modify | configure verb uses preset `dev` (single canonical iterate preset) |
| `scripts/dev-setup.md` | rewrite sections | document the *working* mechanism and the verified ccache keys; delete the false SCCACHE_BASEDIR claims |
| `scripts/new-worktree.ps1` | modify | print ccache stats instead of sccache; no functional change otherwise |

## Task 1: Switch the compiler-cache launcher from sccache to ccache

**Files:**
- Modify: `CMakeLists.txt` (root, lines 33-46, the sccache block)

**Interfaces:**
- Produces: cache-launcher auto-wiring that later tasks' presets rely on; option name `ATX_COMPILER_CACHE` (`AUTO|ccache|sccache|OFF`).
- Consumes: nothing from other tasks.

- [ ] **Step 1: Replace the sccache block in the root `CMakeLists.txt`**

Replace lines 33-46 (`# ---- Compiler cache (sccache) ----` through its `endif()`) with:

```cmake
# ---- Compiler cache (ccache preferred, sccache fallback) --------------------
# ccache (>=4.13) caches clang-cl including MSVC-style PCH TUs (/Yc /Yu /Fp),
# which sccache refuses (447 "non-cacheable /Fp" calls in the 2026-07-12 stats).
# Cross-worktree object reuse needs the CCACHE_* env keys set by the presets
# (BASEDIR/NOHASHDIR/SLOPPINESS/IGNOREOPTIONS) + the -ffile-prefix-map below;
# see scripts/dev-setup.md "Verified cache mechanics". No-op when neither tool
# is installed, so a bare `cmake` still configures.
set(ATX_COMPILER_CACHE "AUTO" CACHE STRING
    "Compiler cache launcher: AUTO (ccache then sccache), ccache, sccache, or OFF")
if(NOT ATX_COMPILER_CACHE STREQUAL "OFF" AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(_atx_cache_candidates ccache sccache)
    if(NOT ATX_COMPILER_CACHE STREQUAL "AUTO")
        set(_atx_cache_candidates ${ATX_COMPILER_CACHE})
    endif()
    find_program(ATX_CACHE_EXE NAMES ${_atx_cache_candidates})
    if(ATX_CACHE_EXE)
        set(CMAKE_C_COMPILER_LAUNCHER   "${ATX_CACHE_EXE}" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${ATX_CACHE_EXE}" CACHE STRING "" FORCE)
        message(STATUS "atx: compiler cache enabled (${ATX_CACHE_EXE})")
    endif()
endif()
```

Keep the old `ATX_USE_SCCACHE` option working as a deprecation shim immediately after the block above:

```cmake
# Deprecated alias (pre-2026-07 presets/scripts): ATX_USE_SCCACHE=OFF disables caching.
option(ATX_USE_SCCACHE "DEPRECATED: use ATX_COMPILER_CACHE=OFF" ON)
if(NOT ATX_USE_SCCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER "" CACHE STRING "" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "" CACHE STRING "" FORCE)
endif()
```

- [ ] **Step 2: Verify configure picks ccache**

Run: `pwsh scripts/atx-build.ps1 --preset dev -B build-cachecheck -S .` — actually use the plain form: `pwsh scripts/atx-build.ps1 "-DATX_BUILD_TESTS=OFF -S . -B build-cachecheck --preset dev"` is awkward; simplest exact command:

```powershell
pwsh scripts/atx-build.ps1 configure
```

Expected in output: `atx: compiler cache enabled (C:/atx-cache/bin/ccache.exe)`.
(This reconfigures the existing `build/` dir; that is intended — Task 3 makes `configure` use the `dev` preset, but at this point it still says `ninja`, which is fine for the check.)

- [ ] **Step 3: Commit**

```powershell
git add CMakeLists.txt
git commit -m @'
build: prefer ccache over sccache as compiler-cache launcher

sccache marks every PCH-using TU non-cacheable (447 /Fp + 3 /Yc calls in
stats), so all PCH-enabled targets bypassed the cache. ccache 4.13 caches
MSVC-style PCH TUs and supports base_dir path normalization for
cross-worktree hits (verified byte-identical objects across worktrees).
ATX_COMPILER_CACHE=AUTO|ccache|sccache|OFF; sccache remains the fallback.
'@
```

## Task 2: Wire the verified ccache keys + shared vcpkg into the presets

**Files:**
- Modify: `CMakePresets.json`

**Interfaces:**
- Consumes: launcher auto-wiring from Task 1.
- Produces: `_base` preset env keys every later task (and every agent worktree) inherits; `dev` preset that `atx-build.ps1` (Task 3) targets.

- [ ] **Step 1: Edit `_base` in `CMakePresets.json`**

In the `_base` preset:

1. Replace the `environment` block:

```json
"environment": {
  "CCACHE_BASEDIR": "${sourceDir}",
  "CCACHE_NOHASHDIR": "1",
  "CCACHE_SLOPPINESS": "pch_defines,time_macros",
  "CCACHE_IGNOREOPTIONS": "-ffile-prefix-map=* /clang:-ffile-prefix-map=*",
  "SCCACHE_BASEDIR": "${sourceDir}"
}
```

(Keep `SCCACHE_BASEDIR` harmlessly for the sccache-fallback path.)

2. Add to `_base.cacheVariables`:

```json
"VCPKG_INSTALLED_DIR": "C:/atx-cache/vcpkg_installed"
```

⚠ Verify during this task that vcpkg accepts a shared installed dir across build trees (it is the documented `VCPKG_INSTALLED_DIR` toolchain variable; the manifest hash check makes the second configure a fast no-op). If a full reinstall happens on the second worktree anyway, keep the setting (it is still one copy on disk instead of N) and note the behavior in dev-setup.md.

3. Update the `_base.description` to name ccache and the four keys (replace the sccache/SCCACHE_BASEDIR prose, which describes a mechanism that never worked).

- [ ] **Step 2: Make the shared-deps behavior default instead of `dev`-only**

Move `"FETCHCONTENT_BASE_DIR": "$env{ATX_DEPS_DIR}"` from the `dev` preset into `_base.cacheVariables` — BUT a preset cache var that expands to an empty string breaks FetchContent, so guard it in the root `CMakeLists.txt` instead. Concretely:

1. In `CMakePresets.json`: delete the `FETCHCONTENT_BASE_DIR` entry from `dev`.
2. In root `CMakeLists.txt`, immediately before `include(FetchContent)`:

```cmake
# Shared FetchContent clone/build cache: every preset (not just `dev`) reuses
# $env{ATX_DEPS_DIR} (C:\atx-cache\deps) when it is set, so a fresh worktree
# never re-clones spdlog/eigen/xsimd/... Falls back to the standard per-build
# _deps dir when the env var is absent (CI, bare cmake).
if(DEFINED ENV{ATX_DEPS_DIR} AND NOT FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "$ENV{ATX_DEPS_DIR}" CACHE PATH "shared FetchContent dir")
endif()
```

3. In `CMakePresets.json`, `dev` keeps only: explicit launcher lines removed too (Task 1 auto-wiring covers it). `dev` becomes:

```json
{
  "name": "dev",
  "inherits": "_base",
  "displayName": "dev: canonical fast-iterate preset (ccache + shared deps + PCH)",
  "description": "The one preset agents and worktrees use. Identical to `ninja` except kept as the stable name scripts target. Unity OFF: per-TU objects are what make the ccache cross-worktree cache hit.",
  "cacheVariables": { "ATX_UNITY_BUILD": "OFF" }
}
```

(`ATX_UNITY_BUILD` was ON in `dev` before; with a working object cache, unity batches HURT: they merge 16 test files into one TU whose hash changes whenever any one file changes, and the engine-test unity batches are the 130-220s worst TUs in the ninja log. Unity remains available via `-DATX_UNITY_BUILD=ON` for cache-less cold CI builds.)

- [ ] **Step 3: Reconfigure and verify env keys reach ccache**

```powershell
pwsh scripts/atx-build.ps1 configure
ccache -z
pwsh scripts/atx-build.ps1 build atx-vol
ccache -s
```

Expected: `ccache -s` shows nonzero `Hits`+`Misses` (not `Uncacheable`), and a second identical `build atx-vol` after `touch`-ing one `atx-vol/src/*.cpp` file shows a miss only for that TU.

- [ ] **Step 4: Commit**

```powershell
git add CMakePresets.json CMakeLists.txt
git commit -m @'
build: verified ccache worktree keys in presets; shared vcpkg + deps dirs

CCACHE_BASEDIR/NOHASHDIR/SLOPPINESS/IGNOREOPTIONS are the empirically
verified minimum for cross-worktree cache hits with clang-cl (see
dev-setup.md). VCPKG_INSTALLED_DIR moves the 1.1 GB per-worktree
vcpkg_installed payload to one shared copy. FETCHCONTENT_BASE_DIR now
applies to every preset via an ATX_DEPS_DIR fallback in CMakeLists, so
the ninja/dev preset split can no longer silently drop the shared caches.
Unity OFF in dev: stable per-TU objects are what the object cache keys on.
'@
```

## Task 3: Unify the script surface on the `dev` preset

**Files:**
- Modify: `scripts/atx-build.ps1` (configure verb + ctest dir)
- Modify: `scripts/new-worktree.ps1` (stats print)
- Modify: `scripts/dev-setup.ps1` (ccache install/config)

**Interfaces:**
- Consumes: `dev` preset shape from Task 2.
- Produces: the single agent-facing workflow (`new-worktree.ps1` → `atx-build.ps1`) later docs describe.

- [ ] **Step 1: `atx-build.ps1` — configure with the `dev` preset**

Change line 54 from:

```powershell
  $cfg = "cmake --preset ninja"
```

to:

```powershell
  $cfg = "cmake --preset dev"
```

(Both presets share `binaryDir` `build/`, so `build`/`-Ctest` verbs need no change. This removes the footgun where `configure` silently reverted a `dev`-configured worktree to per-worktree deps.)

- [ ] **Step 2: `dev-setup.ps1` — install ccache and set its global config**

Add after the existing sccache section (keep sccache install for fallback):

```powershell
# ---- ccache (preferred compiler cache; sccache kept as fallback) ----
$ccacheVer = '4.13.6'
$ccacheExe = Join-Path $CacheBin 'ccache.exe'
if (-not (Test-Path $ccacheExe)) {
  $zip = Join-Path $env:TEMP "ccache-$ccacheVer.zip"
  Invoke-WebRequest -Uri ("https://github.com/ccache/ccache/releases/download/v$ccacheVer/ccache-$ccacheVer-windows-x86_64.zip") -OutFile $zip
  Expand-Archive -Path $zip -DestinationPath $env:TEMP -Force
  Copy-Item (Join-Path $env:TEMP "ccache-$ccacheVer-windows-x86_64\ccache.exe") $ccacheExe
}
& $ccacheExe --set-config cache_dir=C:\atx-cache\ccache
& $ccacheExe --set-config max_size=40G
Write-Host ('ccache ready: ' + (& $ccacheExe --version | Select-Object -First 1))
```

(`$CacheBin` = `C:\atx-cache\bin`, matching the existing sccache fallback install path in that script — reuse whatever variable name the script already has for it; if it hardcodes the path, hardcode the same way.) The per-build keys (BASEDIR/NOHASHDIR/SLOPPINESS/IGNOREOPTIONS) intentionally live in the preset env, not global config, so a build works even on a machine that never re-ran dev-setup.

- [ ] **Step 3: `new-worktree.ps1` — swap the stats print**

Replace line 58:

```powershell
  if (Get-Command sccache -ErrorAction SilentlyContinue) { sccache --show-stats | Select-Object -First 12 }
```

with:

```powershell
  if (Get-Command ccache -ErrorAction SilentlyContinue) { ccache -s | Select-Object -First 10 }
  elseif (Get-Command sccache -ErrorAction SilentlyContinue) { sccache --show-stats | Select-Object -First 12 }
```

- [ ] **Step 4: Run the scripts to verify they parse and act (PowerShell 5.1)**

```powershell
powershell -NoProfile -Command "scripts\dev-setup.ps1"
pwsh scripts/atx-build.ps1 configure
```

Expected: dev-setup completes without error, prints `ccache ready: ccache version 4.13.6`; configure output says `--preset dev` and finishes.

- [ ] **Step 5: Commit**

```powershell
git add scripts/atx-build.ps1 scripts/dev-setup.ps1 scripts/new-worktree.ps1
git commit -m 'build: unify scripts on the dev preset; dev-setup installs ccache'
```

## Task 4: Move gtest discovery off the build critical path

**Files:**
- Modify: `atx-vol/tests/CMakeLists.txt:155-160`
- Modify: `atx-engine/tests/CMakeLists.txt:135`
- Modify: `atx-core/tests/CMakeLists.txt` (its single `gtest_discover_tests` call)
- Modify: `atx-tsdb/tests/CMakeLists.txt` (same)
- Modify: `atx-impl/tests/CMakeLists.txt` (same)

**Interfaces:** none consumed/produced; independent of Tasks 1-3.

- [ ] **Step 1: Add `DISCOVERY_MODE PRE_TEST` to every `gtest_discover_tests` call**

atx-vol (both calls):

```cmake
gtest_discover_tests(atx-vol-tests
    TEST_FILTER "-${ATX_VOL_SLOW_FILTER}"
    PROPERTIES LABELS atx_vol_fast
    DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(atx-vol-tests
    TEST_FILTER "${ATX_VOL_SLOW_FILTER}"
    PROPERTIES LABELS atx_vol_slow
    DISCOVERY_MODE PRE_TEST)
```

Other four files: append `DISCOVERY_MODE PRE_TEST` to the existing call the same way (atx-engine's is `gtest_discover_tests(${_tgt})` → `gtest_discover_tests(${_tgt} DISCOVERY_MODE PRE_TEST)`).

Rationale comment to add once per file (one line): `# PRE_TEST: enumerate at ctest time, not as a POST_BUILD step — relinking no longer pays a test-enumeration run (and a non-launchable exe no longer fails the *build*).`

- [ ] **Step 2: Rebuild + run the fast label to prove discovery still works**

```powershell
pwsh scripts/atx-build.ps1 build atx-vol-tests
pwsh scripts/atx-build.ps1 -Ctest -L atx_vol_fast
```

Expected: build completes with NO `Auto-discovering tests` step in the ninja output; ctest enumerates and runs the atx_vol_fast suite with the same test count as before (compare against `build/Testing/Temporary/LastTest.log` history if in doubt). The fast/slow label split must still hold: `ctest --print-labels` lists `atx_vol_fast` and `atx_vol_slow`.

- [ ] **Step 3: Commit**

```powershell
git add atx-vol/tests/CMakeLists.txt atx-engine/tests/CMakeLists.txt atx-core/tests/CMakeLists.txt atx-tsdb/tests/CMakeLists.txt atx-impl/tests/CMakeLists.txt
git commit -m 'build: gtest discovery PRE_TEST — enumeration off the build critical path'
```

## Task 5: Prime the cache and prove the fresh-worktree win end-to-end

**Files:**
- Create: `docs/superpowers/plans/2026-07-12-build-system-worktree-speed-results.md` (measurements)

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Prime — warm build in the main tree (this is NOT the forbidden "measure the cold baseline"; it is the one-time cache prime the design requires)**

```powershell
ccache -z
pwsh scripts/atx-build.ps1 configure
pwsh scripts/atx-build.ps1 build atx-vol-tests atx-core-tests
ccache -s
```

Expected: misses dominate (first population), `Uncacheable` near zero (PCH TUs now cacheable). Record wall time and `ccache -s` output.

- [ ] **Step 2: Fresh-worktree timing (the actual acceptance gate)**

```powershell
Measure-Command { scripts\new-worktree.ps1 -Name cachegate -Branch chore/cachegate -Base main }
cd ..\atx-wt\cachegate
Measure-Command { pwsh scripts\atx-build.ps1 build atx-vol-tests }
ccache -s
```

Acceptance: the worktree build of `atx-vol-tests` completes in **≤ 10 minutes wall** (target ~3-5) with ccache direct-mode hits for every atx-core/atx-vol lib TU (only PCH-consumer test TUs and the PCH itself compile), vs the multi-tens-of-minutes status quo. If hits are NOT happening: diff hash inputs via `CCACHE_DEBUG=1` exactly as done in the diagnosis (the three env keys in `_base` are the verified fix; check they reached the compile via `build.ninja` env or `set` in the shell).

- [ ] **Step 3: Clean up the probe worktree, write results doc**

```powershell
cd C:\atx
git worktree remove ..\atx-wt\cachegate --force
git branch -D chore/cachegate
```

Write `docs/superpowers/plans/2026-07-12-build-system-worktree-speed-results.md` with: prime wall time, fresh-worktree wall time, `ccache -s` before/after, and the ninja-log CPU totals from the Diagnosis section as the baseline reference.

- [ ] **Step 4: Commit**

```powershell
git add docs/superpowers/plans/2026-07-12-build-system-worktree-speed-results.md
git commit -m 'docs: fresh-worktree build timing results for the ccache overhaul'
```

## Task 6: Rewrite dev-setup.md to describe the mechanism that actually works

**Files:**
- Modify: `scripts/dev-setup.md`

**Interfaces:** consumes final preset/script shape from Tasks 1-4; measurement numbers from Task 5.

- [ ] **Step 1: Rewrite the stale sections**

Replace the "How it works", "Cross-worktree first-party cache hits (the key unlock)" and "Presets" sections. Required content (write it out; keep the table format of the existing doc):

1. Mechanism table rows: ccache object cache (global, `C:\atx-cache\ccache`); the four `CCACHE_*` preset env keys and what each one removes from the hash; shared `VCPKG_INSTALLED_DIR` (1.1 GB + install time per worktree removed); shared `FETCHCONTENT_BASE_DIR` now on ALL presets via `ATX_DEPS_DIR`; LLD/link-pool/module-scan/Z7 rows unchanged.
2. An explicit correction note: "sccache + `SCCACHE_BASEDIR` never provided cross-worktree hits (`SCCACHE_BASEDIR` is not implemented in sccache ≤0.15) and sccache cannot cache PCH TUs at all (`/Fp`). ccache replaces it; sccache remains a fallback launcher only."
3. PCH semantics paragraph: PCH TUs hit the cache **within** a worktree, miss **across** worktrees (clang `.pch` bytes embed absolute paths) — a fresh worktree recompiles the ~230 PCH-consumer test TUs once at PCH speed; everything else is a cross-worktree hit. `hygiene` preset unchanged, still the include-hygiene gate.
4. Preset list: `dev` is THE iterate preset (scripts use it); `ninja` = same but kept for muscle memory; unity note (OFF by default, opt-in for cache-less cold builds).
5. Update the "Inspect cache effectiveness" line to `ccache -s`.

- [ ] **Step 2: Verify doc accuracy against the implementation**

Re-read the final `CMakePresets.json` and root `CMakeLists.txt`; confirm every env key, path, and preset name in the doc matches the code. No stale `SCCACHE_BASEDIR`-as-mechanism claims may survive outside the correction note.

- [ ] **Step 3: Commit**

```powershell
git add scripts/dev-setup.md
git commit -m 'docs: dev-setup describes the verified ccache mechanism; corrects sccache-basedir myth'
```

## Out of Scope (recorded follow-ups, do not implement here)

- **Header-cost reduction** (Eigen leaking through calibrator headers, 45-58s TUs): needs `-ftime-trace` + ClangBuildAnalyzer pass and interface surgery; separate plan.
- **`dev-shared` DLL default for worktrees** (5 GB → ~1 GB build dirs, faster relinks): still marked EXPERIMENTAL; validate a full build+ctest under `dev-shared`, then flip `new-worktree.ps1`'s default in its own change.
- **C++20 modules**: CMake+Ninja support exists but clang-cl + MSVC-ABI module ecosystem is not production-stable for a /WX codebase; revisit ≥2027.
- **Remote/distributed cache** (`ccache` remote storage / sccache-dist): single-machine fleet today; revisit if agents move to multiple hosts.

## Self-Review

- Spec coverage: broken cross-worktree cache → Tasks 1-2; per-worktree dep tax → Task 2; script fragmentation → Task 3; discovery on build path → Task 4; proof + docs → Tasks 5-6. User's "research SOTA" → Diagnosis + Out-of-Scope reflect it; "don't cold-compile first" honored (Task 5 primes warm, measures the worktree side only).
- Placeholders: none; every step has exact code/commands.
- Type/name consistency: `ATX_COMPILER_CACHE` (T1) referenced nowhere else by old name; `dev` preset shape (T2) is what T3 scripts target; `DISCOVERY_MODE PRE_TEST` self-contained.
