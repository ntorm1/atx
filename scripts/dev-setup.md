# Fast iterative dev across git worktrees

Goal: a fresh worktree is reproducible and clangd works immediately — no figuring out
compile commands and no re-cloning/re-building heavy dependencies each time. Compiler-cache
reuse is opportunistic and must be measured for the selected preset.

## How it works (shared caches, not copied build trees)

| Mechanism | What it removes | Where it lives |
|---|---|---|
| **sccache** compiler cache (auto-wired when on PATH; `dev` sets it explicitly) | recompiling cacheable compiler calls; current clang-cl PCH-dependent calls are not cacheable | global cache (`SCCACHE_DIR`, default `%LOCALAPPDATA%`) |
| **`SCCACHE_BASEDIR=${sourceDir}` + `-ffile-prefix-map`** | normalizes worktree paths for calls that sccache can cache | per-build env (set by the preset) + root `CMakeLists.txt` |
| **Shared FetchContent** (`FETCHCONTENT_BASE_DIR=$ATX_DEPS_DIR`) | re-cloning + first-build of spdlog/gtest/benchmark/eigen/xsimd/... per worktree | `C:\atx-cache\deps` |
| **vcpkg binary cache** (Arrow/Parquet/zstd/gtest/openssl) | rebuilding those heavy deps per worktree | vcpkg's global binary cache |
| committed `.clangd` (`CompilationDatabase: build`) | wiring `compile_commands.json` per worktree | each worktree's own `build/` |

### Cross-worktree compiler-cache limit

Build trees are **not** shared (absolute paths are baked in). Two settings normalize paths for
compiler calls that sccache accepts:

- **`SCCACHE_BASEDIR=${sourceDir}`** (preset `environment`) — sccache hashes input paths relative
  to *this worktree's own root*, so they match across worktrees.
- **`-ffile-prefix-map=<repo root>=.`** (root `CMakeLists.txt`, all clang builds) — strips the
  build root from `__FILE__` / debug paths so the emitted object is identical too.

This does not make the current PCH build mostly cache hits. With clang-cl, `/Yu` compilations also
carry `/Fp`; sccache 0.15 reports those calls as non-cacheable. A verified fresh `dev` atx-vol build
reported an 18.17% C/C++ hit rate and 299 `/Fp` non-cacheable calls. Treat shared FetchContent and
vcpkg artifacts as the reliable cross-worktree acceleration, and inspect `sccache --show-stats`
before making cache-performance claims.

### Other build-speed defaults (all presets, root `CMakeLists.txt`)

- **LLD linker** (`ATX_USE_LLD`, on) — `lld-link` relinks the ~16 first-party exes far faster than
  MSVC `link.exe`.
- **Module scan off** (`CMAKE_CXX_SCAN_FOR_MODULES OFF`) — atx uses no C++20 modules, so the
  per-TU Ninja module-dependency scan is removed.
- **Link job pool** (`atx_link`, depth 4) — caps concurrent links so a "build all tests" doesn't
  launch 14 Arrow-pulling links at once and thrash RAM.
- **Embedded debug info** (`/Z7` via `CMP0141=NEW`) — debug objects stay content-cacheable by
  sccache (separate `/Zi` PDBs are not).

## One-time setup (per machine)

```powershell
scripts\dev-setup.ps1      # sets ATX_DEPS_DIR + SCCACHE_CACHE_SIZE, installs sccache
```
Then open a **new shell** so the env/PATH changes apply. (sccache install tries winget, then
falls back to the GitHub release binary into `C:\atx-cache\bin` if winget's source service is
disabled.)

Also required (already set up for this repo): `VCPKG_ROOT` for Arrow/Parquet/zstd
(`find_package` — vcpkg keeps its own global binary cache, so those build once across worktrees).

## Per worktree

From PowerShell (the worktree script delegates configure to `atx-build.ps1`, which
loads the MSVC environment and resolves Ninja + `mt.exe`):

```powershell
scripts\new-worktree.ps1 -Name s8 -Branch feat/s8 -Base main
```
This does `git worktree add`, initializes the required Databento submodule, then
configures the `dev` preset through the build helper. clangd works the moment configure
finishes. Build / test inside it:

```powershell
scripts/atx-build.ps1 -Preset dev build atx-vol-tests
scripts/atx-build.ps1 -Preset dev build atx-engine-<group>-tests
scripts/atx-build.ps1 -Preset dev -Ctest -R <Suite>
```

Inspect cache effectiveness any time with `sccache --show-stats`; record hits, misses, and
non-cacheable reasons for the actual preset/target.

## Priming

The first configured worktree populates shared dependency caches and any cacheable compiler
objects. Later worktrees still build their own PCH and may compile most first-party TUs.

## Presets

- `ninja`      — default Ninja + clang-cl. PCH + LLD + module-scan-off + sccache (auto-detected);
  self-contained static build, deps cloned into its own `build/_deps`.
- `dev`        — `ninja` + explicit sccache + **shared deps** (`ATX_DEPS_DIR`) + unity test builds.
  Use this for iterative worktree work.
- `dev-shared` — `dev` + atx libraries built as **DLLs** (`ATX_SHARED_LIBS`): each test exe links a
  thin import lib instead of embedding the whole engine → fastest relinks and far smaller per-worktree
  build dirs (one engine DLL, not ~14 static copies). Experimental — validate a full build the first
  time (uses `WINDOWS_EXPORT_ALL_SYMBOLS`).
- `rel`        — canonical portable Release acceptance and benchmark build in `build-rel/`.
- `rel-avx2`   — opt-in global `/arch:AVX2` Release comparison in `build-rel-avx2/`; label its baselines.
- `hygiene`    — PCH **off** (strict per-TU includes) for CI/nightly; own `build-hygiene/` dir.
- `vs`         — Visual Studio 2022 MSBuild generator (IDE escape hatch).

### When build artifacts get too big / links feel slow

Switch the worktree to `dev-shared` (`scripts/atx-build.ps1 -Preset dev-shared configure`). The static `dev`/`ninja` build
links a full copy of `atx-engine.lib` into all ~14 test exes; the shared build keeps one
`atx-engine.dll` and links import stubs — typically several GB smaller per worktree and much faster
to relink after a one-file engine edit.
