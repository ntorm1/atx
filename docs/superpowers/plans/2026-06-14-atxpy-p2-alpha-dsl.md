# atxpy P2 — Alpha DSL Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`).

**Goal:** Let Python users (a) compile an alpha expression string and evaluate it over a research panel → a (dates × instruments) signal matrix, and (b) run a compiled alpha AS the backtest strategy (replacing the scripted signal) via `run_backtest(alpha="...")`.

**Architecture:** P2a binds the DSL pipeline 1:1 — `Library`, `parse_expr`, `analyze`, `compile`, plus `Panel::create` / `Engine` / `SignalSet`, and a one-shot `compile_alpha()` convenience. P2b extends the existing `BacktestShim`: `BacktestParams` gains `alpha_expr`; when set, the shim compiles it and swaps `ScriptedSignalSource` for `VmSignalSource` (now the signal member is a `unique_ptr<ISignalSource>`), sizing the rolling panel to the program's `required_lookback + 1`.

**Tech Stack:** C++20, pybind11/numpy, atx-engine alpha module, pandas, pytest.

**Signatures (verbatim, gathered 2026-06-14):**
- `atx::engine::alpha`: `class Library { Library(); const OpSig* find(string_view) const; }`.
- `Result<Ast> parse_expr(string_view source, const Library& lib)` (parser.hpp). `Ast` borrows `const OpSig*` from `lib`; movable, not copyable. `Result<Ast> parse_program(string_view, const Library&)`.
- `Result<Analysis> analyze(const Ast& ast)`; `Analysis::required_lookback() -> u16`.
- `Result<Program> compile(const Ast& ast, const Analysis& analysis)`; `struct Program { vector<Instr> code; vector<Root> roots; vector<string> fields; u32 num_slots; u16 required_lookback; double cache_hit_pct() const; }`; `Program::Root{string name; u32 output;}`.
- `static Result<Panel> Panel::create(usize dates, usize instruments, vector<string> field_names, vector<vector<f64>> field_data, vector<uint8_t> universe)` — **date-major** columns (cell (field,date,inst) at flat `date*instruments+inst`); `universe` empty ⇒ all in. Accessors `dates()`, `instruments()`, `num_fields()`, `Result<FieldId> field_id(name)`.
- `class Engine { explicit Engine(const Panel&); Result<SignalSet> evaluate(const Program&); }`.
- `struct SignalSet { struct Alpha{string name; vector<f64> values;}; vector<Alpha> alphas; usize dates, instruments; span<const f64> alpha_cross_section(usize a, DateIdx date) const; }` — `values` date-major.
- `atx::engine::VmSignalSource : ISignalSource { explicit VmSignalSource(alpha::Program); Result<SignalView> evaluate(PanelView); usize max_lookback() const; }` — transposes the live newest-first PanelView into a chronological alpha::Panel with OHLCV fields, evaluates root 0, returns the newest cross-section.
- Canonical fields (datafields.hpp): `close, volume, high, low, vwap, dollar_volume, adv{d}`.

---

## File Structure

- Create: `python/src/_bindings/bind_alpha.cpp` — `Library`, `Ast`, `Analysis`, `Program`, `parse_expr`, `analyze`, `compile`, `compile_alpha`; `Panel`(opaque) + `evaluate_program(...)` returning a (dates×instruments) NumPy array.
- Modify: `python/src/_bindings/bind_core.cpp` — call `bind_alpha(m)`.
- Modify: `python/src/_bindings/shim/backtest_shim.hpp` — add `std::string alpha_expr;` to `BacktestParams`.
- Modify: `python/src/_bindings/shim/backtest_shim.cpp` — `compile_program()` helper; `signal` → `unique_ptr<ISignalSource>`; VM vs scripted selection; lookback sizing; thread the compiled program through `make_runner`/`BacktestRunner`/`BacktestEnv`.
- Modify: `python/CMakeLists.txt` — add `bind_alpha.cpp`.
- Create: `python/src/atxpy/alpha.py` — `compile_alpha(expr)`, `evaluate(expr_or_program, panel) -> np.ndarray` (panel = dict[field -> 2D array] or pandas).
- Modify: `python/src/atxpy/__init__.py` — re-export alpha facade + `alpha=` kwarg passthrough in `run_backtest`.
- Modify: `python/src/atxpy/backtest.py` — accept `alpha=` (sets `params.alpha_expr`); `signals` optional when `alpha` given.
- Modify: `python/src/atxpy/_core.pyi` — stubs.
- Create: `python/tests/test_alpha_dsl.py`, `python/tests/test_alpha_strategy.py`.

---

## Task 1: Bind the DSL pipeline (parse → analyze → compile)

**Files:** Create `bind_alpha.cpp`; Modify `bind_core.cpp`, `CMakeLists.txt`; Test `test_alpha_dsl.py` (part 1).

- [ ] **Step 1: Failing test** (`python/tests/test_alpha_dsl.py`):
```python
import pytest
from atxpy import _core


def test_compile_alpha_simple():
    prog = _core.compile_alpha("rank(close)")
    assert prog.required_lookback == 0
    assert prog.num_slots >= 1
    assert "close" in prog.fields


def test_compile_alpha_timeseries_lookback():
    prog = _core.compile_alpha("delta(close, 5)")
    assert prog.required_lookback >= 5


def test_parse_error_raises():
    with pytest.raises(_core.AtxError):
        _core.compile_alpha("close +")  # malformed
```

- [ ] **Step 2: Run, verify FAIL.**

- [ ] **Step 3: Write `bind_alpha.cpp`** — bind:
  - `py::class_<alpha::Library>(m,"Library").def(py::init<>())`.
  - `py::class_<alpha::Ast>(m,"Ast")` (opaque; no public methods needed yet beyond holding it).
  - `py::class_<alpha::Analysis>(m,"Analysis").def_property_readonly("required_lookback",[](const Analysis&a){return a.required_lookback();})`.
  - `py::class_<alpha::Program>(m,"Program")` with readonly `required_lookback`, `num_slots`, `fields` (vector<string>), `cache_hit_pct()`, and a `roots` property returning the root names (`[r.name for r in roots]`).
  - free fns: `m.def("parse_expr",[](const std::string&s,const alpha::Library&l){return atxpy::unwrap(alpha::parse_expr(s,l));}, keep_alive<0,2>())` — Ast borrows lib, so `py::keep_alive<0,2>()` ties the returned Ast's lifetime to the Library arg.
  - `m.def("analyze",[](const alpha::Ast&a){return atxpy::unwrap(alpha::analyze(a));})`.
  - `m.def("compile",[](const alpha::Ast&a,const alpha::Analysis&an){return atxpy::unwrap(alpha::compile(a,an));})`.
  - `m.def("compile_alpha",[](const std::string&src){ alpha::Library lib; auto ast=atxpy::unwrap(alpha::parse_expr(src,lib)); auto an=atxpy::unwrap(alpha::analyze(ast)); return atxpy::unwrap(alpha::compile(ast,an)); })` — one-shot; the local Library outlives the call (Ast consumed within).

  > `unwrap` from `result.hpp` raises `AtxError`. Includes: `atx/engine/alpha/{parser,typecheck,bytecode,registry,panel}.hpp`.

- [ ] **Step 4: Register** `void bind_alpha(py::module_&);` + `bind_alpha(m);` in `bind_core.cpp`. Add TU to `CMakeLists.txt`.

- [ ] **Step 5: Build + test** → PASS.

- [ ] **Step 6: Commit** — `feat(atxpy): bind alpha DSL parse/analyze/compile`.

---

## Task 2: Evaluate a program over a research panel (NumPy out)

**Files:** Modify `bind_alpha.cpp`; Test `test_alpha_dsl.py` (part 2).

- [ ] **Step 1: Failing test** (append):
```python
import numpy as np
from atxpy import _core


def test_evaluate_rank_close():
    # 3 dates x 3 instruments; rank(close) on the last date should be 0,0.5,1
    closes = np.array([[10., 20., 30.],
                       [11., 19., 33.],
                       [12., 25., 31.]], dtype=np.float64)
    prog = _core.compile_alpha("rank(close)")
    out = _core.evaluate_program(prog, {"close": closes})
    assert out.shape == (3, 3)
    last = out[-1]
    # 12 < 25 < 31  -> ranks 0, 0.5, 1
    assert np.allclose(np.argsort(np.argsort(last)) / 2.0, last)
```

- [ ] **Step 2: Run, verify FAIL.**

- [ ] **Step 3: Add `evaluate_program` to `bind_alpha.cpp`**:
```cpp
m.def("evaluate_program",
  [](const alpha::Program& prog, const std::map<std::string, py::array_t<double>>& fields) {
    // Determine dates/instruments from the first field; all must match.
    if (fields.empty()) throw py::value_error("need at least one field column");
    const auto& first = fields.begin()->second;
    if (first.ndim() != 2) throw py::value_error("each field must be a 2D [dates,instruments] array");
    const atx::usize dates = static_cast<atx::usize>(first.shape(0));
    const atx::usize inst  = static_cast<atx::usize>(first.shape(1));
    std::vector<std::string> names; std::vector<std::vector<double>> cols;
    for (const auto& [name, arr] : fields) {
      auto a = arr.unchecked<2>();
      if (static_cast<atx::usize>(a.shape(0)) != dates || static_cast<atx::usize>(a.shape(1)) != inst)
        throw py::value_error("all field arrays must share shape");
      std::vector<double> col(dates*inst);
      for (atx::usize d=0; d<dates; ++d) for (atx::usize j=0; j<inst; ++j)
        col[d*inst+j] = a(static_cast<py::ssize_t>(d), static_cast<py::ssize_t>(j));
      names.push_back(name); cols.push_back(std::move(col));
    }
    alpha::Panel panel = atxpy::unwrap(alpha::Panel::create(dates, inst, names, cols, {}));
    alpha::Engine engine{panel};
    alpha::SignalSet sigs = atxpy::unwrap(engine.evaluate(prog));
    // Root 0 -> (dates x inst) NumPy array.
    py::array_t<double> out({static_cast<py::ssize_t>(dates), static_cast<py::ssize_t>(inst)});
    auto v = out.mutable_unchecked<2>();
    const auto& vals = sigs.alphas.at(0).values; // date-major
    for (atx::usize d=0; d<dates; ++d) for (atx::usize j=0; j<inst; ++j)
      v(static_cast<py::ssize_t>(d), static_cast<py::ssize_t>(j)) = vals[d*inst+j];
    return out;
  }, py::arg("program"), py::arg("fields"),
  "Evaluate root 0 of a compiled program over a dict of {field: 2D [dates,instruments]} -> (dates,instruments) array.");
```
> Needs `#include <pybind11/stl.h>` (map), `<pybind11/numpy.h>`, `<map>`, `<string>`, `<vector>`.

- [ ] **Step 4: Build + test** → PASS.

- [ ] **Step 5: Commit** — `feat(atxpy): evaluate compiled alpha over a research panel`.

---

## Task 3: VM strategy inside the backtest (shim refactor)

**Files:** Modify `shim/backtest_shim.hpp` + `.cpp`; Test `test_alpha_strategy.py`.

- [ ] **Step 1: Failing test** (`python/tests/test_alpha_strategy.py`):
```python
import numpy as np
from atxpy import _core


def _two_name_bars(n=8):
    p = _core.BacktestParams()
    p.symbols = ["AAA", "BBB"]
    bars_list = []
    for s in range(2):
        b = _core.BarsForSymbol()
        b.ts_nanos = [i * 86_400_000_000_000 for i in range(n)]
        b.open = [100.0] * n
        b.high = [101.0] * n
        b.low = [99.0] * n
        # AAA trends up, BBB trends down -> a momentum alpha goes long AAA / short BBB
        b.close = [100.0 + (i if s == 0 else -i) for i in range(n)]
        b.volume = [1_000_000.0] * n
        bars_list.append(b)
    p.bars = bars_list
    p.starting_cash = "1000000.0"
    p.every = 1
    return p


def test_alpha_strategy_runs():
    p = _two_name_bars(8)
    p.alpha_expr = "rank(delta(close, 1))"   # cross-sectional momentum
    r = _core.run_backtest(p)
    assert r.slices == 8
    _, fqty, *_ = r.fills_columns()
    assert fqty.size >= 1                     # the DSL strategy actually traded


def test_alpha_strategy_deterministic():
    p = _two_name_bars(8)
    p.alpha_expr = "rank(delta(close, 1))"
    a = _core.run_backtest(p)
    b = _core.run_backtest(p)
    assert a.final_cash.raw() == b.final_cash.raw()
```

- [ ] **Step 2: Run, verify FAIL** (`alpha_expr` attribute missing).

- [ ] **Step 3: Add `alpha_expr` to `BacktestParams`** (hpp): `std::string alpha_expr;` (empty ⇒ scripted path).

- [ ] **Step 4: Refactor the shim** (`.cpp`):
  - Add includes: `atx/engine/alpha/{parser,typecheck,bytecode,registry}.hpp`, `<optional>`, `<algorithm>`, `<memory>`, and `atx/engine/loop/signal_source.hpp` already present (it declares `VmSignalSource`).
  - Add helper:
```cpp
atx::engine::alpha::Program compile_program(const std::string &src) {
  using namespace atx::engine::alpha;
  Library lib;
  auto ast = parse_expr(src, lib);
  if (!ast) throw std::runtime_error("alpha parse: " + ast.error().to_string());
  auto an = analyze(*ast);
  if (!an) throw std::runtime_error("alpha analyze: " + an.error().to_string());
  auto prog = compile(*ast, *an);
  if (!prog) throw std::runtime_error("alpha compile: " + prog.error().to_string());
  return std::move(*prog);
}
```
  - Change `BacktestEnv`: replace `ScriptedSignalSource signal;` with `std::unique_ptr<ISignalSource> signal;` (add `using atx::engine::ISignalSource; using atx::engine::VmSignalSource;`). Its ctor gains `(const BacktestParams& p, atx::usize lookback, std::optional<atx::engine::alpha::Program> prog)`; build `signal` via:
```cpp
signal{prog ? std::unique_ptr<ISignalSource>(std::make_unique<VmSignalSource>(std::move(*prog)))
            : std::unique_ptr<ISignalSource>(std::make_unique<ScriptedSignalSource>(p.signals, p.symbols.size(), lookback))}
```
  (Move the `prog` capture carefully — in a member-initializer the optional is consumed once.)
  - `BacktestRunner<Cap>` ctor gains the same `(p, lookback, prog)`; build `panel{span{env.universe}, lookback}` and the loop with `*env.signal`.
  - `make_runner`:
```cpp
std::optional<atx::engine::alpha::Program> prog;
atx::usize lookback = p.max_lookback;
if (!p.alpha_expr.empty()) {
  atx::engine::alpha::Program program = compile_program(p.alpha_expr);
  lookback = std::max<atx::usize>(p.max_lookback,
                                  static_cast<atx::usize>(program.required_lookback) + 1U);
  prog = std::move(program);
}
if (lookback == 0) throw std::runtime_error("max_lookback must be >= 1");
// dispatch on `lookback`, forwarding (p, lookback, std::move(prog)) into BacktestRunner<Cap>.
```
  Each dispatch line becomes `return std::make_unique<BacktestRunner<N>>(p, lookback, std::move(prog));`.

- [ ] **Step 5: Build + test** → PASS. (Pure C++ + shim change; rebuild required.)

- [ ] **Step 6: Commit** — `feat(atxpy): run a compiled alpha as the backtest strategy (VmSignalSource)`.

---

## Task 4: pandas facade for the DSL + alpha strategy

**Files:** Create `python/src/atxpy/alpha.py`; Modify `backtest.py`, `__init__.py`, `_core.pyi`; Test `test_alpha_facade.py`.

- [ ] **Step 1: Failing test** (`python/tests/test_alpha_facade.py`):
```python
import numpy as np
import pandas as pd
import atxpy


def test_evaluate_facade_dict():
    closes = np.array([[10., 20., 30.], [11., 19., 33.], [12., 25., 31.]])
    out = atxpy.evaluate_alpha("rank(close)", {"close": closes})
    assert out.shape == (3, 3)


def test_run_backtest_with_alpha():
    rows = []
    for s, sym in enumerate(["AAA", "BBB"]):
        for i in range(8):
            rows.append(dict(symbol=sym, timestamp=i * 86_400_000_000_000,
                             open=100., high=101., low=99.,
                             close=100. + (i if s == 0 else -i), volume=1e6))
    bars = pd.DataFrame(rows)
    res = atxpy.run_backtest(bars, signals=None, symbols=["AAA", "BBB"],
                             alpha="rank(delta(close, 1))")
    assert res.slices == 8
    assert len(res.fills) >= 1
```

- [ ] **Step 2: Run, verify FAIL.**

- [ ] **Step 3: Write `python/src/atxpy/alpha.py`**:
```python
"""Facade for the alpha DSL: compile + evaluate over a research panel."""
from __future__ import annotations
import numpy as np
from . import _core


def compile_alpha(expr: str) -> "_core.Program":
    return _core.compile_alpha(expr)


def evaluate_alpha(expr_or_program, panel) -> np.ndarray:
    """Evaluate an alpha over a research panel.

    expr_or_program: a DSL string or a compiled _core.Program.
    panel: dict {field_name: 2D ndarray [dates, instruments]}.
    Returns a (dates, instruments) float64 ndarray (root 0).
    """
    prog = _core.compile_alpha(expr_or_program) if isinstance(expr_or_program, str) else expr_or_program
    fields = {k: np.ascontiguousarray(v, dtype=np.float64) for k, v in panel.items()}
    return _core.evaluate_program(prog, fields)
```

- [ ] **Step 4: Extend `backtest.py`** — add `alpha=None` kwarg; allow `signals=None` when `alpha` set:
  - if `alpha` is not None: set `p.alpha_expr = str(alpha)`; skip the `signals` packing (leave `p.signals` empty).
  - else require `signals` (current behaviour).
  Guard: `if alpha is None and signals is None: raise ValueError("provide signals= or alpha=")`.

- [ ] **Step 5: Re-export** in `__init__.py`: `from .alpha import compile_alpha, evaluate_alpha`; add to `__all__`.

- [ ] **Step 6: Run** → PASS (rebuild only if `_core` changed; here it didn't — pure Python).

- [ ] **Step 7: Commit** — `feat(atxpy): pandas facade for alpha DSL + alpha-as-strategy`.

---

## Task 5: Stubs + full suite + close

- [ ] **Step 1:** Extend `_core.pyi` with `Library`, `Ast`, `Analysis`, `Program`, `parse_expr`, `analyze`, `compile`, `compile_alpha`, `evaluate_program`, and the new `BacktestParams.alpha_expr` field.
- [ ] **Step 2:** `python -m pytest python/tests -v` → all green (P0+P1+P2).
- [ ] **Step 3: Commit** — `docs(atxpy): P2 stubs; close alpha DSL phase`.

---

## Self-Review
- **Coverage (design §9 P2):** Library/parse/analyze/compile ✔, Panel + Engine eval ✔, VmSignalSource as strategy ✔, pandas facade ✔.
- **Lifetime:** `parse_expr` returns an `Ast` borrowing the `Library` → `py::keep_alive<0,2>` ties them; `compile_alpha` keeps the Library on the C++ stack for the whole pipeline so no dangling. `VmSignalSource` owns its `Program` (moved); the shim owns the `VmSignalSource` via `unique_ptr<ISignalSource>` — still nothing non-owning crosses into Python.
- **Lookback correctness:** VM panel sized to `required_lookback + 1` (current bar + required priors) so the newest cross-section is computable; Cap dispatch rounds up.
- **No placeholders:** `evaluate_program` and the shim refactor are given in full; remaining bindings are mechanical and specified field-by-field.
- **Determinism:** the VM/oracle are pure functions of the panel; tests assert byte-identical repeats.

## Open follow-ups
- Expose multi-root programs (currently root 0 only for both eval and strategy).
- `parse_program` (multi-binding) binding for libraries of named alphas (feeds P3 combine).
- Zero-copy field ingest for `evaluate_program` (skip the per-cell copy) once correctness holds.
