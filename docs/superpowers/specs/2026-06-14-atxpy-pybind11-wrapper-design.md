# atxpy — pybind11 Python wrapper for atx-engine

**Status:** Approved (architecture decisions confirmed with user 2026-06-14; design self-approved under autonomous goal directive).
**Date:** 2026-06-14
**Owner:** Nathan Tormaschy

## 1. Goals & non-goals

**Goal:** A high-quality, Pythonic, incrementally-complete binding of atx-engine's
research → backtest → mine → optimize pipeline. NumPy/pandas-native at the boundary.
Preserves the engine's determinism and no-look-ahead guarantees.

**Non-goals (initially, phase-gated late or out of scope):**
- Binding internal helper types (only user-facing entry points).
- The process-spawn `ProcessExecutor` / shared-memory parallel path (last phase, optional).
- Live-trading router / event round-trip (backtest mode only).
- Fully-portable manylinux/Windows redistribution wheels (an explicit hardening phase, not blocking dev use).

## 2. Confirmed architecture decisions (user-approved 2026-06-14)

1. **Delivery shape:** Phased, backtest-first. Vertical slices, each shippable & tested.
2. **Data interop:** NumPy (hard dep) + pandas (helpers). Zero-copy arrays for stable owned buffers; DataFrame helpers for panel construction and result output.
3. **API style:** Pythonic facade over a thin 1:1 core.
4. **Packaging:** New top-level `python/` package, scikit-build-core + pybind11, pip-installable wheel.

## 3. Package layout

```
python/                         # new top-level package root
  pyproject.toml                # scikit-build-core build backend
  CMakeLists.txt                # builds atx libs in-tree, links atx::engine
  src/atxpy/
    __init__.py                 # re-exports + __version__
    _core.pyi                   # type stubs for the compiled extension
    backtest.py  alpha.py       # pythonic facade layer (added phase by phase)
    factory.py   risk.py  ...
    _numpy.py                   # array<->span adapters, pandas helpers
  src/_bindings/                # C++ pybind11 translation units (thin core)
    module.cpp                  # PYBIND11_MODULE root + submodule registration
    bind_core.cpp  bind_loop.cpp  bind_alpha.cpp  ...   # one TU per engine module
    shim/                       # C++ shims that erase templates / fix lifetimes
      backtest_shim.hpp/.cpp    # runtime Cap dispatch, owns the collaborator graph
  tests/                        # pytest: parity vs C++ golden, determinism, lifetime
```

Compiled module = `atxpy._core` (thin 1:1). Hand-written `atxpy.*` modules = Pythonic
facade. `.pyi` stubs ship for IDE/type-checking.

## 4. Two-layer binding model

- **Thin core (`atxpy._core`):** pybind11 mirrors C++ types/signatures 1:1.
  `atx::core::Result<T,E>` (tl::expected) unwraps to a value or raises `atxpy.AtxError`.
  No ergonomics. One `.cpp` per engine module, each kept small to match the codebase's
  file-discipline (a TU that grows large is doing too much).
- **Pythonic facade (`atxpy`):** keyword args, snake_case, context managers, NumPy/pandas
  in-and-out, docstrings. This is what users import. It hides the lifetime footguns (§6).

## 5. Template erasure — `RollingPanel<Cap>` / `BacktestLoop<Cap>`

`Cap` is a compile-time power-of-two ring capacity (the panel's max lookback rounded up).
pybind11 cannot bind a class template directly.

**Plan:** explicitly instantiate a fixed set of caps `{8,16,32,64,128,256,512,1024,4096}`
behind a C++ `BacktestShim`. At runtime the shim rounds the requested `max_lookback` up to
the next power-of-two and dispatches to that instantiation via a `switch`. Python never sees
`Cap`; there is one `atxpy.Backtest` class and capacity is a constructor `int`. A request
exceeding the largest precompiled cap raises a clear error (extend the set if needed).

## 6. Lifetime safety — the core quality risk

`BacktestLoop` holds **non-owning pointers** to ~9 collaborators (feed, clock, bus, panel,
signal, policy, exec, portfolio, market); all must outlive it. Several returned spans dangle
on the next mutation (`AlphaStore::pnl()`, `ISignalSource::evaluate()`'s `SignalView`,
`Genome` borrowing `OpSig*` from the run-wide `Library`).

**Strategy:**
- The `BacktestShim` **owns all collaborators as members**. Python constructs one `Backtest`;
  the shim assembles the object graph internally. No loose non-owning pointers cross into Python.
- pybind11 `py::keep_alive<>` on every binding where one wrapper borrows another
  (`Library` → `Program`, `Panel` → `SignalSet`, etc.).
- Dangling-span returns are **copied into owned NumPy arrays** at the boundary, never exposed
  as views. Zero-copy is reserved for stable, owned buffers (e.g. Panel field columns).
- Lifetime stress tests (drop the Python parent ref mid-use; must not crash) prove the
  keep_alive wiring.

## 7. Data interchange (NumPy + pandas)

- **In:** A research `Panel` / `FeatureMatrix` is built from a pandas DataFrame
  (`dates × instruments` per field) or a dict-of-NumPy-arrays, packed into
  `alpha::Panel::create` / `FeatureMatrix`. Inputs validated: `dtype=float64`, C-contiguous,
  consistent shape, finite-or-NaN.
- **Out:** `BacktestResult.equity_curve` and `.fills` → pandas DataFrames; signals/weights →
  NumPy `float64` arrays (zero-copy where the underlying buffer is owned & stable, else copied).
- **`Decimal`:** atx-core's exact nano-fixed-point (`i64` mantissa, scale 1e-9) is bound as a
  real `atxpy.Decimal` with `from_str` / `__float__` / arithmetic. Money stays exact and is
  never silently converted to float at the boundary.

## 8. Build (scikit-build-core)

- `pyproject.toml` backend = `scikit-build-core`. `python/CMakeLists.txt` does
  `add_subdirectory(<repo root>)` to build `atx-core` / `atx-tsdb` / `atx-engine` static libs
  in-tree, then `pybind11_add_module(_core ...)` linking `atx::engine`.
- The binding TUs do **not** use the `atx_warnings` `/WX` gate (pybind11 headers would trip it);
  they compile with a relaxed warning profile.
- pybind11 fetched via CMake `FetchContent` (pinned tag).
- **vcpkg toolchain required:** atx-core PUBLIC-links `Arrow::arrow_shared` +
  `Parquet::parquet_shared` (and PRIVATE-links databento/zstd/openssl/sqlite/miniz), all
  resolved through `find_package(... CONFIG REQUIRED)` from vcpkg. `CMAKE_TOOLCHAIN_FILE` is
  passed through `pyproject` `[tool.scikit-build.cmake.args]` / env. Dev install = `pip install -e .`.
- **Redistribution:** bundling the Arrow/Parquet runtime DLLs/SOs into a portable wheel
  (delvewheel / auditwheel repair) is a later, explicitly-flagged hardening phase — not
  required for in-tree dev use.

## 9. Phasing (each phase = shippable + pytest-green)

- **P0 — substrate & build:** package skeleton; scikit-build-core compiles an empty `_core`;
  CI smoke import; `Decimal` / `Symbol` / `Bar` vocabulary bound + tested.
- **P1 — backtest vertical:** `InMemoryBarFeed` (bars in from a DataFrame), `BacktestShim`
  (Cap dispatch, owns the graph), `ScriptedSignalSource`, `ExecutionSimulator` + cost configs,
  `Portfolio`, `WeightPolicy`, `Market`, `BacktestResult` → DataFrame. Determinism parity test
  against a C++ golden.
- **P2 — alpha DSL:** `Library`, `parse_expr`, `analyze`, `Program::compile`, `Panel` from
  DataFrame, `oracle::evaluate`, `VmSignalSource` → run a DSL strategy through P1's backtest.
- **P3 — research:** `Factory.mine` + `FactoryReport`; `learn` (`FeatureMatrix`, CPCV folds,
  seed derivation); `combine` (`AlphaCombiner` / `AlphaStore` / `AlphaGate`).
- **P4 — portfolio / eval / book:** `PortfolioOptimizer` + `FactorModel`; `eval` metrics
  (Sharpe / Sortino / PBO / CPCV / deflated-Sharpe); `book` / `fund` meta-allocator.
  Parallel `ProcessExecutor` last / optional.

## 10. Testing strategy

- **Parity:** each bound entry point gets a pytest asserting scalar/array-identical output vs a
  small C++ golden, reusing existing test-fixture expected values where they exist.
- **Determinism:** same input → identical result across two Python calls, matching the C++
  digest where one is published.
- **Lifetime:** stress tests that drop Python parent refs mid-use must not crash (keep_alive proof).
- **Docs-as-tests:** each facade module ships a doctest-style usage example that doubles as
  documentation.

## 11. Open risks

- Arrow/Parquet runtime coupling makes the first portable wheel non-trivial (mitigated by
  phasing redistribution last).
- The precompiled Cap set bounds max lookback; chosen set covers realistic daily-bar lookbacks,
  extendable.
- Some engine entry points return borrowed spans with subtle validity windows; the copy-at-
  boundary rule (§6) is the blanket mitigation, applied per-binding.
