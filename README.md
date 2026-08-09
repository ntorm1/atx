# atx

C++20 quantitative-research monorepo. Every subproject is a static library with
its own `include/atx/<name>/`, `src/` and `tests/`, added in dependency order by
the top-level `CMakeLists.txt`.

## Layout

```
atx/
├── CMakeLists.txt   # standards, GoogleTest, options, subprojects, install/export
├── atx-core/        # numerics, containers, IO, Result/Status error vocabulary
├── atx-vol/         # equity-options pricing + volatility fitting (see its README)
├── atx-tsdb/        # zero-copy time-series panel store
├── atx-kb/          # knowledge base
├── atx-agent-db/    # C++ SQLite-backed agent coordination
├── atx-db/          # Python US-equity data warehouse and research platform
├── atx-engine/      # backtest engine, alpha DSL, risk
├── atx-impl/        # strategy implementations
├── atx-ui/          # optional; gated behind ATX_BUILD_UI
├── python/          # atxpy bindings for the engine stack
├── scripts/         # atx-build.ps1 and friends
└── tests/           # repo-wide test support linked by every subproject's tests
```

`atx-vol` is the module with a published v1 API and distribution policy; start
at [`atx-vol/README.md`](atx-vol/README.md).

## Build

Presets are the supported entry point (`dev`, `rel`, `rel-avx2`, `dev-counters`,
`dev-shared`, `hygiene`):

```bash
cmake --preset dev
cmake --build build
```

On Windows, `scripts/atx-build.ps1` wraps this with the MSVC environment already
set up.

## Test

```bash
ctest --test-dir build --output-on-failure
```

`-L atx_vol` selects the atx-vol label; `ctest -N` lists without running.

## Options

- `ATX_BUILD_TESTS` (default `ON` when top-level) — build tests and fetch GoogleTest.
- `ATX_BUILD_TOOLS` (default follows `PROJECT_IS_TOP_LEVEL`) — the operator CLIs.
- `ATX_BUILD_EXAMPLES` / `ATX_BUILD_BENCH` (default `OFF`) — demos and benchmarks.
- `ATX_BUILD_UI` — the optional `atx-ui` subproject.
- `ATX_SHARED_LIBS` — developer convenience only; not distributable, and
  `cmake --install` of a shared build is refused. See atx-vol's README for why.

GoogleTest is pulled via `FetchContent` (pinned `v1.15.2`); no system install required.
