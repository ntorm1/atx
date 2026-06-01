# atx

C++ monorepo scaffold.

## Layout

```
atx/
├── CMakeLists.txt        # top-level: standards, GoogleTest, subprojects
├── atx-core/             # atx-core static library
│   ├── include/atx/core/
│   ├── src/
│   └── tests/            # atx-core-tests (GoogleTest)
└── atx-engine/           # atx-engine static library (links atx-core)
    ├── include/atx/engine/
    ├── src/
    └── tests/            # atx-engine-tests (GoogleTest)
```

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Options

- `ATX_BUILD_TESTS` (default `ON` when top-level) — build tests and fetch GoogleTest.

GoogleTest is pulled via `FetchContent` (pinned `v1.15.2`); no system install required.
