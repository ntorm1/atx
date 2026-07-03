# Fast iterative dev across git worktrees

Goal: a fresh worktree compiles fast and clangd works immediately — no figuring out
compile commands, no re-cloning/re-building third-party each time, and the *same* source
already compiled in another worktree is a cache **hit**, not a cold recompile.

## How it works (shared caches, not copied build trees)

| Mechanism | What it removes | Where it lives |
|---|---|---|
| **sccache** compiler cache (auto-wired when on PATH; `dev` sets it explicitly) | recompiling the *same* third-party **and** first-party TUs in every worktree | global cache (`SCCACHE_DIR`, default `%LOCALAPPDATA%`) |
| **`SCCACHE_BASEDIR=${sourceDir}` + `-ffile-prefix-map`** | the per-worktree absolute path that used to make first-party objects *miss* across worktrees | per-build env (set by the preset) + root `CMakeLists.txt` |
| **Shared FetchContent** (`FETCHCONTENT_BASE_DIR=$ATX_DEPS_DIR`) | re-cloning + first-build of spdlog/gtest/benchmark/eigen/xsimd/... per worktree | `C:\atx-cache\deps` |
| **vcpkg binary cache** (Arrow/Parquet/zstd/gtest/openssl) | rebuilding those heavy deps per worktree | vcpkg's global binary cache |
| committed `.clangd` (`CompilationDatabase: build`) | wiring `compile_commands.json` per worktree | each worktree's own `build/` |

### Cross-worktree first-party cache hits (the key unlock)

Build trees are **not** shared (absolute paths are baked in). sccache keys an object by the
hashed compiler command + source. Previously the per-worktree absolute path (`C:\atx-wt\s8\...`
vs `C:\atx\...`) leaked into `__FILE__`, debug info, and the input path, so the *same* engine
source **missed** in every new worktree. Two settings fix that and make objects byte-identical
regardless of which worktree they were built in:

- **`SCCACHE_BASEDIR=${sourceDir}`** (preset `environment`) — sccache hashes input paths relative
  to *this worktree's own root*, so they match across worktrees.
- **`-ffile-prefix-map=<repo root>=.`** (root `CMakeLists.txt`, all clang builds) — strips the
  build root from `__FILE__` / debug paths so the emitted object is identical too.

Net effect: prime the cache once (any worktree), and every later worktree's first build is mostly
cache **hits** instead of a cold compile of the ~110 engine TUs.

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

From a **Visual Studio Developer PowerShell** (so the MSVC environment is present):

```powershell
scripts\new-worktree.ps1 -Name s8 -Branch feat/s8 -Base main
```
This does `git worktree add`, then `cmake --preset dev`. clangd works the moment configure
finishes. Build / test inside it:

```powershell
cmake --build --preset dev --target atx-engine-tests
ctest  --preset dev -R <Suite>
```

Inspect cache effectiveness any time: `sccache --show-stats` (watch the hit rate climb across
worktrees).

## Priming

The first `cmake --preset dev` (in your current tree or the first worktree) populates sccache
and the shared dep cache. Every worktree after that is mostly cache hits.

## Presets

- `ninja`      — default Ninja + clang-cl. PCH + LLD + module-scan-off + sccache (auto-detected);
  self-contained static build, deps cloned into its own `build/_deps`.
- `dev`        — `ninja` + explicit sccache + **shared deps** (`ATX_DEPS_DIR`) + unity test builds.
  Use this for iterative worktree work.
- `dev-shared` — `dev` + atx libraries built as **DLLs** (`ATX_SHARED_LIBS`): each test exe links a
  thin import lib instead of embedding the whole engine → fastest relinks and far smaller per-worktree
  build dirs (one engine DLL, not ~14 static copies). Experimental — validate a full build the first
  time (uses `WINDOWS_EXPORT_ALL_SYMBOLS`).
- `hygiene`    — PCH **off** (strict per-TU includes) for CI/nightly; own `build-hygiene/` dir.
- `vs`         — Visual Studio 2022 MSBuild generator (IDE escape hatch).

### When build artifacts get too big / links feel slow

Switch the worktree to `dev-shared` (`cmake --preset dev-shared`). The static `dev`/`ninja` build
links a full copy of `atx-engine.lib` into all ~14 test exes; the shared build keeps one
`atx-engine.dll` and links import stubs — typically several GB smaller per worktree and much faster
to relink after a one-file engine edit.
