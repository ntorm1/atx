# Fast iterative dev across git worktrees

Goal: a fresh worktree compiles fast and clangd works immediately — no figuring out
compile commands, no re-cloning/re-building third-party each time, and the *same* source
already compiled in another worktree is a cache **hit**, not a cold recompile.

## How it works (shared caches, not copied build trees)

| Mechanism | What it removes | Where it lives |
|---|---|---|
| **ccache** compiler cache (auto-wired by the root CMakeLists; sccache is a fallback) | recompiling the *same* third-party **and** first-party TUs — across rebuilds AND across worktrees | global cache `C:\atx-cache\ccache` (set by dev-setup.ps1) |
| **`CCACHE_*` preset env keys** (see "Verified cache mechanics") | the per-worktree absolute paths that would otherwise make every worktree hash differently | `CMakePresets.json` `_base.environment` |
| **Shared vcpkg payload** (`VCPKG_INSTALLED_DIR=C:/atx-cache/vcpkg_installed`) | copying the 1.1 GB Arrow/Parquet/zstd/gtest/openssl install into every worktree's `build/` at configure time | `CMakePresets.json` `_base.cacheVariables` |
| ↳ *caveat* | the installed tree is owned by ONE manifest: if two branches carry **different `vcpkg.json`s**, each configure flips the shared tree (wipe + reinstall from the binary cache). Fine while the manifest is stable; during a dependency-churn sprint, give that worktree its own tree: `cmake --preset dev -DVCPKG_INSTALLED_DIR=build/vcpkg_installed` | [vcpkg #31328](https://github.com/microsoft/vcpkg/discussions/31328) |
| **Shared FetchContent** (`ATX_DEPS_DIR` → `FETCHCONTENT_BASE_DIR`, ALL presets via a root-CMakeLists fallback) | re-cloning + first-build of spdlog/eigen/xsimd/... per worktree | `C:\atx-cache\deps` |
| committed `.clangd` (`CompilationDatabase: build`) | wiring `compile_commands.json` per worktree | each worktree's own `build/` |

### Verified cache mechanics (2026-07-12, do not "simplify")

ccache keys an object by hashed compiler command + preprocessed input. Four settings
make the SAME source hash identically in every worktree (verified: compile in worktree A,
recompile in worktree B = cache hit, objects byte-identical). They live in TWO places
because CMake preset `environment` blocks only apply to `cmake --preset` /
`cmake --build --preset` invocations — a raw `cmake --build build` or bare ninja run
sees none of them. The three worktree-invariant keys are therefore ALSO in the global
ccache config (written by dev-setup.ps1), and `atx-build.ps1` exports the one
per-worktree key (`CCACHE_BASEDIR`) itself:

- **`CCACHE_BASEDIR=${sourceDir}`** — worktree-internal absolute paths (source file, `-I`
  dirs) are hashed relative to *this* worktree's root, so they match across worktrees.
- **`CCACHE_NOHASHDIR=1`** — stops hashing the build cwd (safe: `-ffile-prefix-map`
  already keeps the cwd out of the emitted object).
- **`CCACHE_IGNOREOPTIONS=-ffile-prefix-map=* /clang:-ffile-prefix-map=*`** — the
  prefix-map *argument value* embeds the worktree path; ccache's built-in prefix-map
  exclusion does not fire for the `/clang:`-wrapped spelling clang-cl needs, so it is
  excluded explicitly. The flag itself still reaches the compiler (correctness: objects
  stay path-clean and byte-identical, so serving A's object to B is exact).
- **`CCACHE_SLOPPINESS=pch_defines,time_macros`** — makes PCH-using TUs cacheable.

Two standing rules that keep this safe (research-verified 2026-07-12):

1. `CCACHE_IGNOREOPTIONS` removes options from the HASH — that is only sound for options
   that cannot change codegen. The two prefix-map patterns qualify (they alter embedded
   path strings only, and our mapping makes those identical anyway). **Never add a
   pattern for an option that affects output** (defines, /O flags, -Xclang) — that would
   produce silently wrong objects on a hit, not a cache miss.
2. Every checkout must keep its build dir at the same relative spot (`<root>/build`) —
   BASEDIR rewrites to cwd-relative paths, so a worktree building in `build/foo/` would
   hash differently and never share. All presets pin `${sourceDir}/build`, so this holds
   unless someone hand-rolls `-B`.

**PCH semantics:** TUs compiled against the precompiled header (atx-engine, all test
targets) hit the cache **within** a worktree but miss **across** worktrees — the clang
`.pch` bytes embed absolute paths (`-ffile-prefix-map` does not reach PCH serialization),
so each worktree builds its own PCH and PCH-consumer objects once, at PCH speed
(~3-5s/TU instead of 10-30s). Everything else — the whole engine, atx-core, atx-vol lib,
third-party — transfers across worktrees as byte-identical cache hits.

**History note (why sccache was replaced):** the previous design (sccache +
`SCCACHE_BASEDIR`) never worked: `SCCACHE_BASEDIR` is not implemented in sccache ≤0.15
(stats show `Base directories (none)` however it is set), and sccache marks every
PCH-using TU non-cacheable (447 `/Fp` + 3 `/Yc` rejections in the 2026-07-12 stats), so
all PCH-enabled targets bypassed the cache entirely. ccache does both correctly. sccache
remains only as a fallback launcher when ccache is absent.

### Other build-speed defaults (all presets, root `CMakeLists.txt`)

- **LLD linker** (`ATX_USE_LLD`, on) — `lld-link` relinks the ~16 first-party exes far faster than
  MSVC `link.exe`.
- **Module scan off** (`CMAKE_CXX_SCAN_FOR_MODULES OFF`) — atx uses no C++20 modules, so the
  per-TU Ninja module-dependency scan is removed.
- **Link job pool** (`atx_link`, depth 4) — caps concurrent links so a "build all tests" doesn't
  launch 14 Arrow-pulling links at once and thrash RAM.
- **Embedded debug info** (`/Z7` via `CMP0141=NEW`) — debug objects stay content-cacheable
  (separate `/Zi` PDBs are not).
- **Test discovery at ctest time** (`DISCOVERY_MODE PRE_TEST` on every
  `gtest_discover_tests`) — relinking a test exe no longer runs it at *build* time to
  enumerate ~2600 tests, and a non-launchable exe fails the test step, not the build.

## One-time setup (per machine)

```powershell
scripts\dev-setup.ps1      # sets ATX_DEPS_DIR, installs ccache (+ sccache fallback), sets ccache config
```
Then open a **new shell** so the env/PATH changes apply.

Also required (already set up for this repo): `VCPKG_ROOT` for Arrow/Parquet/zstd
(`find_package` — vcpkg keeps its own global binary cache, so those build once per machine).

## Per worktree

From a **Visual Studio Developer PowerShell** (so the MSVC environment is present):

```powershell
scripts\new-worktree.ps1 -Name s8 -Branch feat/s8 -Base main
```
This does `git worktree add`, then `cmake --preset dev`. clangd works the moment configure
finishes. Build / test inside it:

```powershell
cmake --build --preset dev --target atx-vol-tests
ctest  --preset dev -R <Suite>
```

Or use the wrapper that sources vcvars + the VS-bundled Ninja for you:

```powershell
powershell scripts\atx-build.ps1 configure          # cmake --preset dev (same build/ dir)
powershell scripts\atx-build.ps1 build atx-vol-tests
powershell scripts\atx-build.ps1 -Ctest -L atx_vol_fast
```

Inspect cache effectiveness any time: `ccache -s` (watch the hit rate climb across
worktrees; `Uncacheable` should stay near zero — if it grows, a new flag is fighting the
cache, run with `CCACHE_DEBUG=1` and diff the `.ccache-input-text` files).

Cache operations hygiene: the cache lives on local NTFS (`C:\atx-cache\ccache`) — never
relocate it to a network share/OneDrive (direct-mode false-hit reports all trace to
network filesystems), and don't run `ccache -c`/cleanup while parallel worktree builds
are in flight.

## Priming

The first build (any worktree) populates ccache, the shared deps dir, and the shared
vcpkg dir. Every worktree after that: configure is seconds (vcpkg manifest already
satisfied), library/engine TUs are cache hits, and only the worktree's own PCH +
PCH-consumer test TUs actually compile.

## Presets

- `dev`        — **the** iterate preset; what `new-worktree.ps1` and `atx-build.ps1` use.
  ccache + shared deps + shared vcpkg + PCH, unity OFF (stable per-TU objects are what
  the cross-worktree cache keys on).
- `ninja`      — identical inheritance from `_base`; kept as a familiar alias. Same `build/` dir.
- `dev-shared` — `dev` + atx libraries built as **DLLs** (`ATX_SHARED_LIBS`): each test exe links a
  thin import lib instead of embedding the whole engine → fastest relinks and far smaller per-worktree
  build dirs. Experimental — validate a full build the first time (uses `WINDOWS_EXPORT_ALL_SYMBOLS`).
- `rel` / `rel-avx2` — Release benchmarks (own build dirs); see preset descriptions.
- `hygiene`    — PCH **off** (strict per-TU includes) for CI/nightly; own `build-hygiene/` dir.
  Also the right preset for `-DATX_UNITY_BUILD=ON` cold cache-less builds.
- `vs`         — Visual Studio 2022 MSBuild generator (IDE escape hatch).

### When build artifacts get too big / links feel slow

Switch the worktree to `dev-shared` (`cmake --preset dev-shared`). The static `dev`/`ninja` build
links a full copy of `atx-engine.lib` into all ~14 test exes; the shared build keeps one
`atx-engine.dll` and links import stubs — typically several GB smaller per worktree and much faster
to relink after a one-file engine edit.
