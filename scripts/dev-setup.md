# Fast iterative dev across git worktrees

Goal: a fresh worktree compiles fast and clangd works immediately — no figuring out
compile commands, no re-cloning/re-building third-party each time.

## How it works (two shared caches, not copied build trees)

| Mechanism | What it removes | Where it lives |
|---|---|---|
| **sccache** compiler cache (`CMAKE_*_COMPILER_LAUNCHER=sccache`) | recompiling the *same* third-party **and** first-party TUs in every worktree | global cache (`SCCACHE_DIR`, default `%LOCALAPPDATA%`) |
| **Shared FetchContent** (`FETCHCONTENT_BASE_DIR=$ATX_DEPS_DIR`) | re-cloning + first-build of spdlog/gtest/benchmark/eigen/xsimd/... per worktree | `C:\atx-cache\deps` |
| committed `.clangd` (`CompilationDatabase: build`) | wiring `compile_commands.json` per worktree | each worktree's own `build/` |

Build trees are **not** shared across worktrees (absolute paths are baked in). Cross-worktree
reuse comes from the object cache (sccache) keyed by hashed preprocessed source — so the same
source in any worktree is a cache hit.

The `dev` preset also forces embedded debug info (`/Z7` via `CMP0141=NEW` +
`CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded`); without it clang-cl emits separate `/Zi`
PDBs that sccache cannot cache.

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

- `ninja` — the original Ninja + clang-cl preset (unchanged; no caching).
- `dev`   — `ninja` + sccache + shared deps. Use this for iterative work.
- `vs`    — Visual Studio 2022 MSBuild generator.
