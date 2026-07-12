# Build-System Worktree-Speed Overhaul — Measured Results (2026-07-12)

Companion to `2026-07-12-build-system-worktree-speed.md`. All numbers measured on the
dev machine (Windows 11, VS2022 clang-cl 18.1.8, ccache 4.13.6, 16 test jobs), target
`atx-vol-tests` (atx-core + atx-vol lib + ~100 test TUs + gtest link) under the `dev`
preset, in the worktree `C:\atx\.claude\worktrees\build-worktree-speed`.

## Baseline (before)

- sccache stats at diagnosis: **450/1145 compile requests non-cacheable** (reasons `/Fp`
  447, `/Yc` 3) — every PCH-using TU bypassed the cache; hit rate 31.6%; measured mean
  compile 32.5 s/TU.
- `SCCACHE_BASEDIR` verified a no-op (sccache 0.15 knows only static-list
  `SCCACHE_BASEDIRS`; stats show `Base directories (none)` regardless) → **zero**
  cross-worktree first-party reuse. Fresh-worktree speed came from PCH alone.
- Ninja-log CPU totals (Release, full build): ~4.9 CPU-hours; atx-vol lib 2525 s + its
  tests 1077 s CPU. 1.1 GB `vcpkg_installed` copied per worktree at configure.

## After

| Scenario | Wall time | Cache behavior |
|---|---|---|
| Cold populate build of `atx-vol-tests` (empty ccache, post-clean) | 698 s first pass / 230 s clean-rebuild pass | 98.85% of calls cacheable (was ~59%); only 3 "preprocessing failed" outliers |
| **Clean → full rebuild, warm cache (the fresh-worktree equivalent for lib TUs)** | **25 s** | **258/258 hits (100.0%)** |
| Configure, shared vcpkg dir already populated | vcpkg step "already installed" in ~15 s | one shared 1.1 GB payload instead of per-worktree copies |
| `ctest -L atx_vol_fast` under PRE_TEST discovery | 58.6 s | 973/973 pass; no build-time enumeration runs |

Cross-worktree object identity was proven directly during diagnosis: the same TU
compiled in two different worktree roots produces **byte-identical objects** and the
second compile is a **cache hit** (CCACHE_BASEDIR + NOHASHDIR + IGNOREOPTIONS on the
`/clang:`-wrapped prefix-map). PCH-consumer TUs are the one same-worktree-only class
(the clang `.pch` embeds absolute paths), so a brand-new worktree recompiles the ~100
test TUs once at PCH speed and hits on everything else.

## Defects found and fixed along the way

1. **PCH × sccache mutual exclusion** — the repo's two headline speed mechanisms
   cancelled each other per-TU. (launcher switched to ccache + sloppiness keys)
2. **`SCCACHE_BASEDIR` no-op** — the documented "key unlock" for worktrees never
   existed. (replaced by verified ccache base_dir design)
3. **Preset env doesn't reach raw builds** — `cmake --build <dir>` (what
   `atx-build.ps1` runs) sees no preset `environment`; 108/269 calls were "could not
   use precompiled header" until the invariant keys moved to global ccache config and
   `atx-build.ps1` began exporting `CCACHE_BASEDIR` itself.
4. **`ninja` vs `dev` preset split** — `atx-build.ps1 configure` silently reconfigured
   dev worktrees without shared deps. (both scripts now target `dev`; `ninja` aliases it)
5. **Fresh-worktree bring-up failures** — `CMAKE_MT-NOTFOUND` (silent vcvars64/vswhere
   failure; SDK bin dir now pinned onto PATH) and empty databento submodule
   (`new-worktree.ps1` now runs `git submodule update --init --recursive`).
6. **Unity batches fighting the object cache** — 16-file test batches were the slowest
   TUs (130-220 s) and any one-file change re-keys the whole batch; unity now OFF in
   `dev`, opt-in for cache-less cold builds.
7. **Test enumeration on the build path** — every relink ran the test exes (atx-vol
   twice) to enumerate ~2600 tests, and a non-launchable exe failed the build.
   (`DISCOVERY_MODE PRE_TEST` everywhere)

## Follow-ups deliberately out of scope

- Header-cost surgery (Eigen-heavy calibrators at 45-58 s/TU): run ClangBuildAnalyzer
  over a `-ftime-trace` build, then targeted forward-decl/interface work.
- `dev-shared` (DLL) as the worktree default after a full validation pass.
- vcpkg-manifest-churn ergonomics: if a sprint edits `vcpkg.json` across branches, give
  that worktree `-DVCPKG_INSTALLED_DIR=build/vcpkg_installed` (see dev-setup.md caveat).
