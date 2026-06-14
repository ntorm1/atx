# atxpy

Python wrapper for atx-engine (quant backtesting engine), built with pybind11.

## Build (development)

Run from a **VS 2022 Developer shell** (so `clang-cl` + the MSVC environment are
present), with `VCPKG_ROOT` set:

```powershell
python -m pip install scikit-build-core pybind11 ninja pytest numpy
python -m pip install -e ./python
python -m pytest python/tests -v
```

`pyproject.toml` already pins the vcpkg toolchain, `x64-windows` triplet, `clang-cl`,
and reuses the monorepo's `build/vcpkg_installed` tree (arrow/parquet/zstd/openssl),
so a plain `pip install -e ./python` configures correctly inside a VS dev shell.

The first build compiles the whole monorepo (atx-core / atx-tsdb / atx-engine) plus
the extension, so expect several minutes cold.

## Layout

- `src/atxpy/` — the Python package (facade, re-exports, type stubs).
- `src/_bindings/` — the C++ pybind11 translation units (thin `_core` module).
- `tests/` — pytest suite (parity, behaviour, lifetime).
