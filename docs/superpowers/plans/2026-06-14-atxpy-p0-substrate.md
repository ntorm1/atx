# atxpy P0 — Substrate & Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the `python/` package so `pip install -e .` compiles a pybind11 extension (`atxpy._core`) linked against `atx::engine`, importable from Python, with the atx-core money/symbol vocabulary (`Decimal`, `SymbolTable`/`Symbol`) bound and pytest-green.

**Architecture:** New top-level `python/` package. `scikit-build-core` drives CMake; `python/CMakeLists.txt` `add_subdirectory`s the monorepo root to build `atx-core`/`atx-tsdb`/`atx-engine` static libs in-tree, then `pybind11_add_module(_core ...)` links `atx::engine`. A thin `_core` extension mirrors C++ 1:1; `atx::core::Result<T>` unwraps to a value or raises `atxpy.AtxError`. Preconditions that ABORT in C++ (out-of-range `from_int`, bad `Symbol`) are guarded in the binding so Python sees exceptions.

**Tech Stack:** C++20, pybind11, scikit-build-core, CMake (Ninja + clang-cl), vcpkg (x64-windows, manifest mode), pytest, NumPy (declared now, used P1+).

**Build environment (hard prerequisite):** All build commands must run from a **VS 2022 Developer PowerShell/CMD** (so `clang-cl` + the MSVC environment + `vcpkg` are present). `VCPKG_ROOT` is already set to `C:\Users\natha\vcpkg`. The extension uses the SAME compiler/toolchain/triplet as `CMakePresets.json` (`clang-cl`, `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`, `x64-windows`) to stay ABI-compatible with the atx static libs.

---

## File Structure

- Create: `python/pyproject.toml` — scikit-build-core backend, deps, scikit-build config (toolchain/compiler/triplet defines).
- Create: `python/CMakeLists.txt` — adds monorepo root, finds pybind11, builds `_core`, installs it into the wheel.
- Create: `python/README.md` — build/install instructions (VS dev shell, vcpkg).
- Create: `python/.gitignore` — `build/`, `dist/`, `*.egg-info`, `__pycache__`, `*.pyd`.
- Create: `python/src/atxpy/__init__.py` — re-exports + `__version__` + `AtxError`.
- Create: `python/src/atxpy/_core.pyi` — type stubs for the compiled module.
- Create: `python/src/_bindings/module.cpp` — `PYBIND11_MODULE(_core, …)`, registers `AtxError`, calls per-module `bind_*` registrars.
- Create: `python/src/_bindings/result.hpp` — `unwrap()` helper turning `Result<T>` into a value or a thrown `AtxError`.
- Create: `python/src/_bindings/bind_core.cpp` — binds `Decimal`, `Symbol`, `SymbolTable`.
- Create: `python/tests/test_smoke.py` — import + version.
- Create: `python/tests/test_decimal.py` — Decimal parity/behaviour.
- Create: `python/tests/test_symbol.py` — SymbolTable behaviour + guard.

---

## Task 0: Prerequisites & sanity (no code)

**Files:** none (environment check).

- [ ] **Step 1: Confirm the build toolchain is reachable**

Run (in a VS Developer shell):
```
clang-cl --version
cmake --version
python --version
echo %VCPKG_ROOT%
```
Expected: clang-cl prints a version, cmake ≥ 3.25, python 3.12.x, `VCPKG_ROOT` = `C:\Users\natha\vcpkg`.
If `clang-cl` is missing, the shell is not a VS Developer shell — open "x64 Native Tools Command Prompt for VS 2022" (which also has clang-cl if the LLVM/clang VS component is installed) or run `scripts/dev-setup.md` guidance.

- [ ] **Step 2: Install Python build tooling**

Run:
```
python -m pip install --upgrade pip
python -m pip install scikit-build-core pybind11 ninja pytest numpy
```
Expected: all install successfully. `ninja` is needed because it is not otherwise on PATH; `pybind11` provides its CMake config for `find_package`.

- [ ] **Step 3: Confirm the monorepo already configures under vcpkg**

Run:
```
cmake --preset ninja
```
Expected: configure succeeds (Arrow/Parquet/zstd resolve from vcpkg). This proves the heavy deps are installed before we add the wheel build on top. (If this fails, fix the base build first — the wheel cannot succeed where the base configure fails.)

---

## Task 1: Package skeleton + empty extension that imports

**Files:**
- Create: `python/.gitignore`
- Create: `python/pyproject.toml`
- Create: `python/CMakeLists.txt`
- Create: `python/src/_bindings/module.cpp`
- Create: `python/src/_bindings/result.hpp`
- Create: `python/src/atxpy/__init__.py`
- Create: `python/README.md`
- Test: `python/tests/test_smoke.py`

- [ ] **Step 1: Write the failing smoke test**

`python/tests/test_smoke.py`:
```python
def test_import_core():
    import atxpy
    from atxpy import _core
    assert hasattr(_core, "__doc__")


def test_version():
    import atxpy
    assert isinstance(atxpy.__version__, str)
    assert atxpy.__version__
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python -m pytest python/tests/test_smoke.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'atxpy'` (not installed yet).

- [ ] **Step 3: Create `python/.gitignore`**

```
build/
dist/
*.egg-info/
__pycache__/
*.pyd
*.so
*.pyc
.pytest_cache/
```

- [ ] **Step 4: Create `python/pyproject.toml`**

```toml
[build-system]
requires = ["scikit-build-core>=0.10", "pybind11>=2.12"]
build-backend = "scikit_build_core.build"

[project]
name = "atxpy"
version = "0.1.0"
description = "Python wrapper for the atx-engine quant backtesting engine"
readme = "README.md"
requires-python = ">=3.10"
authors = [{ name = "Nathan Tormaschy", email = "nathan.tormaschy@gmail.com" }]
dependencies = ["numpy>=1.23"]

[project.optional-dependencies]
pandas = ["pandas>=2.0"]
test = ["pytest>=7", "numpy>=1.23", "pandas>=2.0"]

[tool.scikit-build]
# pyproject dir (python/) is the CMake source dir; it pulls in the monorepo root.
minimum-version = "0.10"
build-dir = "build/{wheel_tag}"
wheel.packages = ["src/atxpy"]
# Surface CMake configure/build logs while we stabilise the toolchain.
cmake.verbose = true
cmake.build-type = "Release"

[tool.scikit-build.cmake.define]
# Match CMakePresets.json so the extension is ABI-compatible with the atx libs.
CMAKE_C_COMPILER = "clang-cl"
CMAKE_CXX_COMPILER = "clang-cl"
CMAKE_TOOLCHAIN_FILE = { env = "VCPKG_ROOT", default = "" }
VCPKG_TARGET_TRIPLET = "x64-windows"
# Engine has no tests/bench when consumed as a subdir; keep them off explicitly.
ATX_BUILD_TESTS = "OFF"
ATX_BUILD_BENCH = "OFF"
ATX_WERROR = "OFF"
```

> Note: `CMAKE_TOOLCHAIN_FILE` needs the full path `$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`. scikit-build-core's `{env}` form yields only `VCPKG_ROOT`; therefore `python/CMakeLists.txt` (Step 5) appends the suffix itself if the value is a bare root. Pass it explicitly on the CLI for the first build:
> `python -m pip install -e . --config-settings=cmake.define.CMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"`

- [ ] **Step 5: Create `python/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.25)
project(atxpy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# If CMAKE_TOOLCHAIN_FILE was passed as a bare VCPKG_ROOT, complete the path.
if(CMAKE_TOOLCHAIN_FILE AND IS_DIRECTORY "${CMAKE_TOOLCHAIN_FILE}")
  set(CMAKE_TOOLCHAIN_FILE
      "${CMAKE_TOOLCHAIN_FILE}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "" FORCE)
endif()

# Build the monorepo (atx-core / atx-tsdb / atx-engine) in-tree. The root is the
# parent of python/; give it an explicit binary dir since it is outside our tree.
# As a subdir it is not top-level, so its tests/bench stay off by default.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/atx-monorepo)

# pybind11 from the pip-installed package (scikit-build-core puts it on the path).
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(_core
    src/_bindings/module.cpp
    src/_bindings/bind_core.cpp
)
target_include_directories(_core PRIVATE src/_bindings)
target_link_libraries(_core PRIVATE atx::engine)
# pybind11 headers trip the engine's /WX gate, so the extension does NOT link
# atx_warnings; it compiles with the default (relaxed) warning profile.

install(TARGETS _core LIBRARY DESTINATION atxpy RUNTIME DESTINATION atxpy)
```

> `bind_core.cpp` is listed now so Task 2/3 only edit it. It must exist for configure to pass; create a minimal stub in Step 7.

- [ ] **Step 6: Create `python/src/_bindings/result.hpp`**

```cpp
#pragma once
// Bridges atx::core::Result<T> (tl::expected<T, Error>) to Python: returns the
// value on Ok, raises atxpy.AtxError on Err. Register the exception once at module
// init (see module.cpp) so the translation has a Python type to throw.

#include <pybind11/pybind11.h>
#include <utility>

#include "atx/core/error.hpp"

namespace atxpy {

// Throws a C++ std::runtime_error carrying the atx error text; pybind11's
// registered exception translator (module.cpp) maps it to atxpy.AtxError.
struct AtxException : std::runtime_error {
  explicit AtxException(const atx::core::Error &e) : std::runtime_error(e.to_string()) {}
};

template <class T>
[[nodiscard]] T unwrap(atx::core::Result<T> r) {
  if (!r) {
    throw AtxException(r.error());
  }
  return std::move(*r);
}

} // namespace atxpy
```

- [ ] **Step 7: Create `python/src/_bindings/module.cpp` and a `bind_core.cpp` stub**

`python/src/_bindings/module.cpp`:
```cpp
#include <pybind11/pybind11.h>

#include "result.hpp"

namespace py = pybind11;

// Each engine module registers its bindings through one of these.
void bind_core(py::module_ &m);

PYBIND11_MODULE(_core, m) {
  m.doc() = "atxpy._core — thin pybind11 bindings over atx-engine.";

  // atxpy.AtxError: every atx::core::Error surfaces here.
  static py::exception<atxpy::AtxException> exc(m, "AtxError");
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) {
        std::rethrow_exception(p);
      }
    } catch (const atxpy::AtxException &e) {
      py::set_error(exc, e.what());
    }
  });

  bind_core(m);
}
```

`python/src/_bindings/bind_core.cpp` (stub — fleshed out in Task 2/3):
```cpp
#include <pybind11/pybind11.h>

namespace py = pybind11;

void bind_core(py::module_ &m) {
  // Vocabulary bindings added in Task 2 (Decimal) and Task 3 (Symbol).
  (void)m;
}
```

- [ ] **Step 8: Create `python/src/atxpy/__init__.py`**

```python
"""atxpy — Python wrapper for the atx-engine quant backtesting engine."""

from __future__ import annotations

from . import _core
from ._core import AtxError

__version__ = "0.1.0"

__all__ = ["_core", "AtxError", "__version__"]
```

- [ ] **Step 9: Create `python/README.md`**

```markdown
# atxpy

Python wrapper for atx-engine.

## Build (development)

Run from a **VS 2022 Developer shell** (clang-cl + MSVC env), with `VCPKG_ROOT` set:

```
python -m pip install scikit-build-core pybind11 ninja pytest numpy
python -m pip install -e ./python ^
  --config-settings=cmake.define.CMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake"
python -m pytest python/tests -v
```

The first build compiles the whole monorepo (atx-core/atx-tsdb/atx-engine) plus
the extension, so expect several minutes cold.
```

- [ ] **Step 10: Build/install the extension**

Run (VS dev shell, from repo root):
```
python -m pip install -e ./python --config-settings=cmake.define.CMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
```
Expected: monorepo + `_core` compile and link; editable install completes. (Cold build is slow.)

- [ ] **Step 11: Run the smoke test to verify it passes**

Run: `python -m pytest python/tests/test_smoke.py -v`
Expected: PASS (2 tests).

- [ ] **Step 12: Commit**

```
git add python/ docs/superpowers/plans/2026-06-14-atxpy-p0-substrate.md
git commit -m "feat(atxpy): P0 package skeleton — scikit-build-core builds importable _core"
```

---

## Task 2: Bind `Decimal`

**Files:**
- Modify: `python/src/_bindings/bind_core.cpp`
- Test: `python/tests/test_decimal.py`

- [ ] **Step 1: Write the failing test**

`python/tests/test_decimal.py`:
```python
import math
import pytest
from atxpy import _core
from atxpy._core import Decimal, AtxError


def test_from_string_roundtrip():
    d = Decimal.from_string("123.456789")
    assert d.to_string() == "123.456789"
    assert math.isclose(float(d), 123.456789, rel_tol=1e-12)


def test_from_int_and_arithmetic():
    a = Decimal.from_int(3)
    b = Decimal.from_string("1.5")
    assert (a + b).to_string() == "4.5"
    assert (a - b).to_string() == "1.5"
    assert (a * b).to_string() == "4.5"
    assert (-b).to_string() == "-1.5"


def test_comparison_and_eq():
    assert Decimal.from_int(2) < Decimal.from_int(3)
    assert Decimal.from_string("1.5") == Decimal.from_string("1.5")
    assert Decimal.from_int(3) >= Decimal.from_string("3.0")


def test_from_string_bad_input_raises():
    with pytest.raises(AtxError):
        Decimal.from_string("not-a-number")


def test_from_int_out_of_range_raises():
    # |whole| > kMaxWhole (9_223_372_036) would ABORT in C++; the binding guards it.
    with pytest.raises((AtxError, ValueError, OverflowError)):
        Decimal.from_int(10_000_000_000)


def test_repr_and_raw():
    d = Decimal.from_string("2.5")
    assert d.raw() == 2_500_000_000
    assert "2.5" in repr(d)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python -m pytest python/tests/test_decimal.py -v`
Expected: FAIL — `AttributeError: module 'atxpy._core' has no attribute 'Decimal'`.

- [ ] **Step 3: Implement the Decimal binding**

Replace `python/src/_bindings/bind_core.cpp` with:
```cpp
#include <pybind11/operators.h>
#include <pybind11/pybind11.h>

#include <string>

#include "atx/core/decimal.hpp"
#include "result.hpp"

namespace py = pybind11;
using atx::core::Decimal;

namespace {

// Guard from_int: C++ ABORTS when |whole| > kMaxWhole. Raise instead.
Decimal decimal_from_int_guarded(atx::i64 whole) {
  if (whole > Decimal::kMaxWhole || whole < -Decimal::kMaxWhole) {
    throw py::value_error("Decimal.from_int out of range (|whole| > kMaxWhole)");
  }
  return Decimal::from_int(whole);
}

void bind_decimal(py::module_ &m) {
  py::class_<Decimal>(m, "Decimal", "Exact fixed-point money (nano scale, 1e-9).")
      .def_static("from_raw", &Decimal::from_raw, py::arg("mantissa"),
                  "Exact construction from a raw i64 mantissa (value = mantissa / 1e9).")
      .def_static("from_int", &decimal_from_int_guarded, py::arg("whole"),
                  "Whole-unit construction; raises on |whole| > kMaxWhole.")
      .def_static("from_double",
                  [](atx::f64 v) { return atxpy::unwrap(Decimal::from_double(v)); },
                  py::arg("value"), "Round a double to the nano grid; raises on NaN/inf/range.")
      .def_static("from_string",
                  [](const std::string &s) { return atxpy::unwrap(Decimal::from_string(s)); },
                  py::arg("text"), "Parse a decimal string; raises AtxError on bad input.")
      .def("raw", &Decimal::raw, "Raw i64 mantissa (value * 1e9).")
      .def("to_double", &Decimal::to_double, "Lossy double (display/heuristics only).")
      .def("to_string", &Decimal::to_string, "Canonical decimal string.")
      .def("round", &Decimal::round, "Round half-away-from-zero to the nearest whole unit.")
      .def(py::self + py::self)
      .def(py::self - py::self)
      .def(py::self * py::self)
      .def(py::self / py::self)
      .def(-py::self)
      .def(py::self == py::self)
      .def(py::self != py::self)
      .def(py::self < py::self)
      .def(py::self <= py::self)
      .def(py::self > py::self)
      .def(py::self >= py::self)
      .def("__float__", &Decimal::to_double)
      .def("__str__", &Decimal::to_string)
      .def("__repr__",
           [](const Decimal &d) { return "Decimal('" + d.to_string() + "')"; })
      .def("__hash__", [](const Decimal &d) { return std::hash<atx::i64>{}(d.raw()); });
}

} // namespace

// Forward decl for the Symbol binding added in Task 3.
void bind_symbol(py::module_ &m);

void bind_core(py::module_ &m) {
  bind_decimal(m);
  bind_symbol(m);
}
```

> `bind_symbol` is referenced now; add its stub at the end of this file so the TU links until Task 3 fills it in:
> ```cpp
> void bind_symbol(py::module_ &m) { (void)m; }
> ```
> Place this stub temporarily; Task 3 replaces it with the real implementation in a new TU and removes the stub.

- [ ] **Step 4: Rebuild and run the test**

Run:
```
python -m pip install -e ./python --config-settings=cmake.define.CMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
python -m pytest python/tests/test_decimal.py -v
```
Expected: PASS (6 tests).

- [ ] **Step 5: Commit**

```
git add python/src/_bindings/bind_core.cpp python/tests/test_decimal.py
git commit -m "feat(atxpy): bind atx-core Decimal (exact money) with guarded factories"
```

---

## Task 3: Bind `Symbol` + `SymbolTable`

**Files:**
- Create: `python/src/_bindings/bind_symbol.cpp`
- Modify: `python/CMakeLists.txt` (add the new TU)
- Modify: `python/src/_bindings/bind_core.cpp` (remove the temporary `bind_symbol` stub)
- Test: `python/tests/test_symbol.py`

- [ ] **Step 1: Write the failing test**

`python/tests/test_symbol.py`:
```python
import pytest
from atxpy._core import Symbol, SymbolTable


def test_intern_is_idempotent():
    t = SymbolTable()
    a1 = t.intern("AAPL")
    a2 = t.intern("AAPL")
    msft = t.intern("MSFT")
    assert a1 == a2
    assert a1 != msft
    assert t.size() == 2


def test_name_roundtrip():
    t = SymbolTable()
    s = t.intern("SPY")
    assert t.name(s) == "SPY"


def test_symbol_id_and_hash():
    t = SymbolTable()
    s = t.intern("QQQ")
    assert isinstance(s.id, int)
    assert hash(s) == hash(t.intern("QQQ"))  # equal symbols hash equal


def test_name_out_of_range_raises():
    # A Symbol not from this table (id >= size) would ABORT in C++; binding guards it.
    t = SymbolTable()
    t.intern("ONE")
    bogus = Symbol(999)
    with pytest.raises((ValueError, IndexError)):
        t.name(bogus)
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python -m pytest python/tests/test_symbol.py -v`
Expected: FAIL — `ImportError: cannot import name 'Symbol'` (or `SymbolTable` missing).

- [ ] **Step 3: Create `python/src/_bindings/bind_symbol.cpp`**

```cpp
#include <pybind11/pybind11.h>

#include <string>
#include <string_view>

#include "atx/core/domain/symbol.hpp"

namespace py = pybind11;
using atx::core::domain::Symbol;
using atx::core::domain::SymbolTable;

void bind_symbol(py::module_ &m) {
  py::class_<Symbol>(m, "Symbol", "Opaque interned instrument id (32-bit).")
      .def(py::init([](atx::u32 id) { return Symbol{id}; }), py::arg("id"))
      .def_readonly("id", &Symbol::id)
      .def(py::self == py::self)
      .def(py::self != py::self)
      .def(py::self < py::self)
      .def("__hash__", [](const Symbol &s) { return std::hash<atx::u32>{}(s.id); })
      .def("__repr__", [](const Symbol &s) { return "Symbol(" + std::to_string(s.id) + ")"; });

  py::class_<SymbolTable>(m, "SymbolTable", "Owns the id<->name interning map (move-only).")
      .def(py::init<>())
      .def("intern",
           [](SymbolTable &t, std::string_view name) { return t.intern(name); },
           py::arg("name"), "Intern a name to a stable Symbol (idempotent).")
      .def("name",
           [](const SymbolTable &t, Symbol s) -> std::string {
             if (s.id >= t.size()) {
               throw py::value_error("Symbol not from this table (id >= size)");
             }
             return std::string{t.name(s)};
           },
           py::arg("symbol"), "Name for a Symbol from this table; raises if out of range.")
      .def("size", &SymbolTable::size, "Number of distinct interned symbols.")
      .def("__len__", &SymbolTable::size);
}
```

> Requires `#include <pybind11/operators.h>` for `py::self`. Add it to the includes.

- [ ] **Step 4: Add the operators include**

At the top of `python/src/_bindings/bind_symbol.cpp`, add after the first include:
```cpp
#include <pybind11/operators.h>
```

- [ ] **Step 5: Remove the temporary stub in `bind_core.cpp`**

Delete the temporary line added in Task 2:
```cpp
void bind_symbol(py::module_ &m) { (void)m; }
```
(The forward declaration `void bind_symbol(py::module_ &m);` stays — the real definition now lives in `bind_symbol.cpp`.)

- [ ] **Step 6: Register the new TU in `python/CMakeLists.txt`**

Change the `pybind11_add_module` source list to:
```cmake
pybind11_add_module(_core
    src/_bindings/module.cpp
    src/_bindings/bind_core.cpp
    src/_bindings/bind_symbol.cpp
)
```

- [ ] **Step 7: Rebuild and run the test**

Run:
```
python -m pip install -e ./python --config-settings=cmake.define.CMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
python -m pytest python/tests/test_symbol.py -v
```
Expected: PASS (4 tests).

- [ ] **Step 8: Commit**

```
git add python/src/_bindings/bind_symbol.cpp python/src/_bindings/bind_core.cpp python/CMakeLists.txt python/tests/test_symbol.py
git commit -m "feat(atxpy): bind Symbol + SymbolTable interning with out-of-range guard"
```

---

## Task 4: Type stubs + facade polish

**Files:**
- Create: `python/src/atxpy/_core.pyi`
- Modify: `python/src/atxpy/__init__.py`
- Test: `python/tests/test_smoke.py` (extend)

- [ ] **Step 1: Extend the smoke test for the public surface**

Append to `python/tests/test_smoke.py`:
```python
def test_public_exports():
    import atxpy
    assert {"Decimal", "Symbol", "SymbolTable", "AtxError"} <= set(dir(atxpy))
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python -m pytest python/tests/test_smoke.py::test_public_exports -v`
Expected: FAIL — names not re-exported from `atxpy` yet.

- [ ] **Step 3: Re-export the vocabulary in `__init__.py`**

Replace `python/src/atxpy/__init__.py`:
```python
"""atxpy — Python wrapper for the atx-engine quant backtesting engine."""

from __future__ import annotations

from . import _core
from ._core import AtxError, Decimal, Symbol, SymbolTable

__version__ = "0.1.0"

__all__ = ["_core", "AtxError", "Decimal", "Symbol", "SymbolTable", "__version__"]
```

- [ ] **Step 4: Create `python/src/atxpy/_core.pyi`**

```python
from typing import overload

class AtxError(Exception): ...

class Decimal:
    @staticmethod
    def from_raw(mantissa: int) -> "Decimal": ...
    @staticmethod
    def from_int(whole: int) -> "Decimal": ...
    @staticmethod
    def from_double(value: float) -> "Decimal": ...
    @staticmethod
    def from_string(text: str) -> "Decimal": ...
    def raw(self) -> int: ...
    def to_double(self) -> float: ...
    def to_string(self) -> str: ...
    def round(self) -> "Decimal": ...
    def __add__(self, other: "Decimal") -> "Decimal": ...
    def __sub__(self, other: "Decimal") -> "Decimal": ...
    def __mul__(self, other: "Decimal") -> "Decimal": ...
    def __truediv__(self, other: "Decimal") -> "Decimal": ...
    def __neg__(self) -> "Decimal": ...
    def __float__(self) -> float: ...
    def __lt__(self, other: "Decimal") -> bool: ...
    def __le__(self, other: "Decimal") -> bool: ...
    def __gt__(self, other: "Decimal") -> bool: ...
    def __ge__(self, other: "Decimal") -> bool: ...

class Symbol:
    def __init__(self, id: int) -> None: ...
    @property
    def id(self) -> int: ...

class SymbolTable:
    def __init__(self) -> None: ...
    def intern(self, name: str) -> Symbol: ...
    def name(self, symbol: Symbol) -> str: ...
    def size(self) -> int: ...
    def __len__(self) -> int: ...
```

- [ ] **Step 5: Mark the package as typed**

Create `python/src/atxpy/py.typed` (empty file) so type-checkers pick up the stubs:
```
```

- [ ] **Step 6: Run the full P0 test suite**

Run: `python -m pytest python/tests -v`
Expected: PASS (all smoke + decimal + symbol tests).

- [ ] **Step 7: Commit**

```
git add python/src/atxpy/__init__.py python/src/atxpy/_core.pyi python/src/atxpy/py.typed python/tests/test_smoke.py
git commit -m "feat(atxpy): re-export vocabulary, ship type stubs + py.typed"
```

---

## Self-Review notes

- **Spec coverage:** P0 scope from the design (§9) = package skeleton, scikit-build-core compiling `_core`, smoke import, `Decimal`/`Symbol` vocabulary bound + tested. All covered (Tasks 1–4). `Bar`/`Price`/`Quantity` are intentionally deferred to P1 (they enter with the feed) — noted in the design; not a P0 gap.
- **Result→exception:** `result.hpp::unwrap` + the module-level translator give `from_double`/`from_string` real `AtxError` behaviour (tested).
- **Abort guards:** `Decimal.from_int` range and `SymbolTable.name` bounds are guarded so Python raises instead of aborting the interpreter (tested).
- **Type consistency:** `bind_core` calls `bind_decimal` + `bind_symbol`; `bind_symbol` is forward-declared in `bind_core.cpp` and defined in `bind_symbol.cpp` (the temporary stub is removed in Task 3 Step 5 — single definition at link time).
- **Toolchain risk:** the `CMAKE_TOOLCHAIN_FILE` bare-root vs full-path issue is handled both in `CMakeLists.txt` (completes a directory value) and via the explicit CLI override in the build command.

## Open follow-ups (not P0)

- Redistributable wheel (bundle Arrow/Parquet via delvewheel) — later hardening phase.
- CI workflow that runs this on a Windows runner with vcpkg cache — after P1 stabilises.
- Faster wheel builds by linking pre-built atx static libs instead of recompiling the monorepo — optimization, only if cold-build time hurts.
