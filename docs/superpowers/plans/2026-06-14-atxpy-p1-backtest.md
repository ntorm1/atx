# atxpy P1 — Backtest Vertical Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`).

**Goal:** Run a full deterministic backtest from Python: bars + a scripted signal schedule in → equity curve, fills, and P&L out (as pandas DataFrames). Hide the templated `RollingPanel<Cap>`/`BacktestLoop<Cap>` behind a runtime-dispatched C++ `BacktestShim`; expose the cost/policy config knobs; preserve the engine's determinism and no-look-ahead guarantees across the boundary.

**Architecture:** A C++ shim owns the entire collaborator graph so no non-owning pointer escapes to Python. `BacktestParams` (a plain value struct filled from Python) → `make_runner()` picks the smallest precompiled power-of-two `Cap ≥ max_lookback` and returns a `unique_ptr<IBacktestRunner>`; `BacktestRunner<Cap>` owns a `BacktestEnv` (feed, clock, bus, signal, policy, exec, portfolio, market + all backing storage) plus `RollingPanel<Cap>` + `BacktestLoop<Cap>`, declared so the loop outlives nothing it points at. `run()` returns the engine's `BacktestResult` by value. Bindings expose result columns as zero-copy-ish NumPy arrays; a Python facade wraps pandas in/out.

**Tech Stack:** C++20, pybind11 (numpy headers), atx-engine, pandas (optional), pytest.

**Inputs known (verbatim signatures gathered 2026-06-14):**
- `InMemoryBarFeed(std::span<const std::span<const BarRow>> sources, SimClock&, EventBus<>&)`; `BarRow{Symbol symbol; Bar bar; Timestamp knowledge_ts; bool delisted_final;}`.
- `RollingPanel<Cap>(std::span<const InstrumentId> universe, usize max_lookback)`; static_assert Cap power-of-two, `max_lookback ≤ Cap`.
- `ScriptedSignalSource(const std::vector<std::vector<f64>>& schedule, usize universe_size, usize max_lookback)`.
- `WeightPolicy{Transform transform=Rank; bool industry_neutral=false; bool dollar_neutral=true; f64 gross_leverage=1.0; f64 truncation=0.0; f64 winsorize_limit=0.025;}`; `enum Transform{Rank,ZScore}`.
- `Market(std::span<const InstrumentId> universe, std::span<const InstrumentStats> stats)`; `InstrumentStats{f64 adv,sigma,spread;}`.
- `Portfolio(Decimal starting_cash, std::span<const InstrumentId> universe)`.
- `ExecutionSimulator(FillCfg, SlippageCfg, ImpactCfg, CommissionCfg, LatencyCfg, VolumeCapCfg)`.
  - `FillCfg{bool allow_same_bar_fill=false;}`
  - `SlippageCfg{SlippageMode mode=VolumeShare; f64 k=0.1,bps=5.0,cap_volshare=0.025,cap_bps=0.10;}`; `enum SlippageMode{VolumeShare,FixedBps}`
  - `ImpactCfg{f64 Y=1.0,delta=0.5,gamma=0.314;}`
  - `CommissionCfg{CommissionMode mode=PerShare; f64 per_share=0.005,min_fee=1.0,max_pct=0.005,per_dollar_bps=15.0;}`; `enum CommissionMode{PerShare,PerDollar}`
  - `LatencyCfg{i64 latency_nanos=0;}`
  - `VolumeCapCfg{f64 volume_limit=0.025;}`
- `BacktestLoop<Cap>(IDataHandler&, SimClock&, EventBus<>&, RollingPanel<Cap>&, ISignalSource&, const WeightPolicy&, ExecutionSimulator&, Portfolio&, Market&, Universe, Schedule, Delay=Next)`; `Schedule{usize every=1}`; `enum Delay{Same,Next}`.
- `BacktestResult{vector<EquitySample> equity_curve; vector<FillPayload> fills; Decimal final_cash; f64 final_equity,turnover; usize slices,rebalances;}`.
- `EquitySample{Timestamp t; f64 equity,gross,net;}`; `FillPayload{Symbol id; i64 qty; Decimal price,fee; f64 impact; Timestamp t;}`; `OrderType{Market,Limit}`.
- `Timestamp::from_unix_nanos(i64)` / `.unix_nanos()`.

---

## File Structure

- Create: `python/src/_bindings/shim/backtest_shim.hpp` — `BacktestParams`, `IBacktestRunner`, `make_runner` decl.
- Create: `python/src/_bindings/shim/backtest_shim.cpp` — `BacktestStorage`/`BacktestEnv`/`BacktestRunner<Cap>` + `make_runner` Cap dispatch + bar construction.
- Create: `python/src/_bindings/bind_config.cpp` — `Transform`, slippage/commission modes, the 6 cost cfgs, `InstrumentStats`, `WeightPolicy`, `OrderType` enums/structs.
- Create: `python/src/_bindings/bind_backtest.cpp` — `BacktestParams` setters, `EquitySample`/`FillPayload`/`BacktestResult` (with NumPy column extractors), `run_backtest(params)`.
- Modify: `python/CMakeLists.txt` — add the 3 new TUs; link nothing new.
- Modify: `python/src/_bindings/bind_core.cpp` — call `bind_config(m)` + `bind_backtest(m)` from `bind_core`.
- Create: `python/src/atxpy/backtest.py` — pandas facade: `run_backtest(bars, signals, ...) -> BacktestResult`.
- Modify: `python/src/atxpy/__init__.py` — re-export the facade + new types.
- Modify: `python/src/atxpy/_core.pyi` — stubs for the new bound types.
- Create: `python/tests/test_backtest_core.py` — C++-level run via `_core.run_backtest`.
- Create: `python/tests/test_backtest_determinism.py` — determinism + no-look-ahead parity.
- Create: `python/tests/test_backtest_facade.py` — pandas in/out.

> Build is unchanged: every build is `pip install -e ./python --no-build-isolation` run inside the VS dev shell (wrapped through `vcvars64.bat`), per P0.

---

## Task 1: Bind config enums + structs

**Files:** Create `python/src/_bindings/bind_config.cpp`; Modify `python/CMakeLists.txt`, `python/src/_bindings/bind_core.cpp`; Test `python/tests/test_config.py`.

- [ ] **Step 1: Failing test** — `python/tests/test_config.py`:
```python
from atxpy import _core


def test_cost_cfg_defaults_and_kwargs():
    s = _core.SlippageCfg()
    assert s.k == 0.1 and s.bps == 5.0
    s.k = 0.2
    assert s.k == 0.2
    c = _core.CommissionCfg()
    assert c.per_share == 0.005
    f = _core.FillCfg()
    assert f.allow_same_bar_fill is False


def test_weight_policy_defaults():
    wp = _core.WeightPolicy()
    assert wp.transform == _core.Transform.Rank
    assert wp.dollar_neutral is True
    assert wp.gross_leverage == 1.0
    wp.transform = _core.Transform.ZScore
    assert wp.transform == _core.Transform.ZScore


def test_instrument_stats():
    st = _core.InstrumentStats()
    st.adv = 1e6
    assert st.adv == 1e6
```

- [ ] **Step 2: Run, verify fail** — `python -m pytest python/tests/test_config.py -v` → FAIL (names missing).

- [ ] **Step 3: Write `python/src/_bindings/bind_config.cpp`**
```cpp
#include <pybind11/pybind11.h>

#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/exec/payloads.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace py = pybind11;
namespace e = atx::engine;
namespace ex = atx::engine::exec;

void bind_config(py::module_ &m) {
  py::enum_<e::Transform>(m, "Transform")
      .value("Rank", e::Transform::Rank)
      .value("ZScore", e::Transform::ZScore);
  py::enum_<ex::SlippageMode>(m, "SlippageMode")
      .value("VolumeShare", ex::SlippageMode::VolumeShare)
      .value("FixedBps", ex::SlippageMode::FixedBps);
  py::enum_<ex::CommissionMode>(m, "CommissionMode")
      .value("PerShare", ex::CommissionMode::PerShare)
      .value("PerDollar", ex::CommissionMode::PerDollar);
  py::enum_<ex::OrderType>(m, "OrderType")
      .value("Market", ex::OrderType::Market)
      .value("Limit", ex::OrderType::Limit);

  py::class_<ex::FillCfg>(m, "FillCfg")
      .def(py::init<>())
      .def_readwrite("allow_same_bar_fill", &ex::FillCfg::allow_same_bar_fill);
  py::class_<ex::SlippageCfg>(m, "SlippageCfg")
      .def(py::init<>())
      .def_readwrite("mode", &ex::SlippageCfg::mode)
      .def_readwrite("k", &ex::SlippageCfg::k)
      .def_readwrite("bps", &ex::SlippageCfg::bps)
      .def_readwrite("cap_volshare", &ex::SlippageCfg::cap_volshare)
      .def_readwrite("cap_bps", &ex::SlippageCfg::cap_bps);
  py::class_<ex::ImpactCfg>(m, "ImpactCfg")
      .def(py::init<>())
      .def_readwrite("Y", &ex::ImpactCfg::Y)
      .def_readwrite("delta", &ex::ImpactCfg::delta)
      .def_readwrite("gamma", &ex::ImpactCfg::gamma);
  py::class_<ex::CommissionCfg>(m, "CommissionCfg")
      .def(py::init<>())
      .def_readwrite("mode", &ex::CommissionCfg::mode)
      .def_readwrite("per_share", &ex::CommissionCfg::per_share)
      .def_readwrite("min_fee", &ex::CommissionCfg::min_fee)
      .def_readwrite("max_pct", &ex::CommissionCfg::max_pct)
      .def_readwrite("per_dollar_bps", &ex::CommissionCfg::per_dollar_bps);
  py::class_<ex::LatencyCfg>(m, "LatencyCfg")
      .def(py::init<>())
      .def_readwrite("latency_nanos", &ex::LatencyCfg::latency_nanos);
  py::class_<ex::VolumeCapCfg>(m, "VolumeCapCfg")
      .def(py::init<>())
      .def_readwrite("volume_limit", &ex::VolumeCapCfg::volume_limit);

  py::class_<e::loop::InstrumentStats>(m, "InstrumentStats")
      .def(py::init<>())
      .def_readwrite("adv", &e::loop::InstrumentStats::adv)
      .def_readwrite("sigma", &e::loop::InstrumentStats::sigma)
      .def_readwrite("spread", &e::loop::InstrumentStats::spread);

  py::class_<e::WeightPolicy>(m, "WeightPolicy")
      .def(py::init<>())
      .def_readwrite("transform", &e::WeightPolicy::transform)
      .def_readwrite("industry_neutral", &e::WeightPolicy::industry_neutral)
      .def_readwrite("dollar_neutral", &e::WeightPolicy::dollar_neutral)
      .def_readwrite("gross_leverage", &e::WeightPolicy::gross_leverage)
      .def_readwrite("truncation", &e::WeightPolicy::truncation)
      .def_readwrite("winsorize_limit", &e::WeightPolicy::winsorize_limit);
}
```

> NOTE: verify the namespace of `InstrumentStats` and `Transform` when implementing — `Transform`/`WeightPolicy` are `atx::engine::` (confirmed: `namespace atx::engine`); `InstrumentStats`/`Market` may be `atx::engine::loop::` or `atx::engine::`. Adjust the `e::` vs `e::loop::` qualifier to match `market.hpp` at implementation time (single source of truth — grep the header).

- [ ] **Step 4: Register in `bind_core.cpp`** — add forward decls + calls:
```cpp
void bind_config(py::module_ &m);
// ... inside bind_core(m): bind_config(m); (before bind_backtest)
```

- [ ] **Step 5: Add TU to `python/CMakeLists.txt`** — append `src/_bindings/bind_config.cpp` to the `pybind11_add_module(_core ...)` source list.

- [ ] **Step 6: Build + test** — `pip install -e ./python --no-build-isolation` (in vcvars shell) then `python -m pytest python/tests/test_config.py -v` → PASS.

- [ ] **Step 7: Commit** — `feat(atxpy): bind cost/policy config structs + enums`.

---

## Task 2: The C++ BacktestShim

**Files:** Create `python/src/_bindings/shim/backtest_shim.hpp` + `.cpp`; Modify `python/CMakeLists.txt`.

This task adds no Python surface yet (Task 3 binds it); it is validated by Task 3's tests. It is one logical unit, so it is one commit.

- [ ] **Step 1: Write `python/src/_bindings/shim/backtest_shim.hpp`**
```cpp
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "atx/core/types.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/backtest_loop.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxpy {

// Plain value struct filled from Python. One BarsForSymbol per universe symbol,
// each a column-wise OHLCV time series in knowledge_ts order.
struct BarsForSymbol {
  std::vector<atx::i64> ts_nanos;
  std::vector<atx::f64> open, high, low, close, volume;
};

struct BacktestParams {
  std::vector<std::string> symbols;          // defines universe column order
  std::vector<BarsForSymbol> bars;           // bars[i] <-> symbols[i]
  std::string starting_cash = "1000000.0";   // exact decimal string
  std::vector<std::vector<atx::f64>> signals; // [rebalance][universe] alpha scores
  atx::usize max_lookback = 1;
  atx::usize every = 1;
  bool delay_same = false;                    // false=Next (firewall), true=Same
  atx::engine::WeightPolicy policy{};
  atx::engine::exec::FillCfg fill{};
  atx::engine::exec::SlippageCfg slip{};
  atx::engine::exec::ImpactCfg impact{};
  atx::engine::exec::CommissionCfg comm{};
  atx::engine::exec::LatencyCfg latency{};
  atx::engine::exec::VolumeCapCfg volcap{};
  std::vector<atx::engine::loop::InstrumentStats> stats{}; // empty or size==symbols
};

// Type-erased runner over the templated RollingPanel<Cap>/BacktestLoop<Cap>.
struct IBacktestRunner {
  virtual ~IBacktestRunner() = default;
  [[nodiscard]] virtual atx::engine::BacktestResult run() = 0;
};

// Picks the smallest precompiled power-of-two Cap >= params.max_lookback.
// Throws std::runtime_error if max_lookback exceeds the largest Cap (4096).
[[nodiscard]] std::unique_ptr<IBacktestRunner> make_runner(const BacktestParams &params);

} // namespace atxpy
```

- [ ] **Step 2: Write `python/src/_bindings/shim/backtest_shim.cpp`**
```cpp
#include "backtest_shim.hpp"

#include <span>
#include <stdexcept>

#include "atx/core/datetime.hpp"
#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/loop/rolling_panel.hpp"
#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/portfolio/portfolio.hpp"

namespace atxpy {
namespace {

using atx::engine::Delay;
using atx::engine::EventBus;
using atx::engine::Market;
using atx::engine::Portfolio;
using atx::engine::Schedule;
using atx::engine::ScriptedSignalSource;
using atx::engine::SimClock;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::data::BarRow;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::loop::InstrumentId;
using atx::engine::loop::InstrumentStats;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::core::domain::SymbolTable;
using atx::core::time::Timestamp;

// Exact Decimal from an f64 OHLC/price input; throws on NaN/inf/range.
Decimal dec(atx::f64 v) {
  auto r = Decimal::from_double(v);
  if (!r) {
    throw std::runtime_error("price/volume not representable as Decimal: " + r.error().to_string());
  }
  return *r;
}

// Storage layer: built FIRST so the collaborators below borrow stable memory.
struct BacktestStorage {
  SymbolTable symbols;
  std::vector<InstrumentId> universe;
  std::vector<std::vector<BarRow>> bar_storage;
  std::vector<std::span<const BarRow>> bar_spans;
  std::vector<InstrumentStats> stats;
  Decimal starting_cash;

  explicit BacktestStorage(const BacktestParams &p) {
    const atx::usize n = p.symbols.size();
    if (p.bars.size() != n) {
      throw std::runtime_error("bars length must equal symbols length");
    }
    universe.reserve(n);
    for (const std::string &name : p.symbols) {
      universe.push_back(symbols.intern(name)); // id i == column i (insertion order)
    }
    bar_storage.resize(n);
    for (atx::usize i = 0; i < n; ++i) {
      const BarsForSymbol &b = p.bars[i];
      const atx::usize rows = b.ts_nanos.size();
      if (b.open.size() != rows || b.high.size() != rows || b.low.size() != rows ||
          b.close.size() != rows || b.volume.size() != rows) {
        throw std::runtime_error("OHLCV column lengths must match ts length");
      }
      bar_storage[i].reserve(rows);
      for (atx::usize r = 0; r < rows; ++r) {
        const Timestamp ts = Timestamp::from_unix_nanos(b.ts_nanos[r]);
        Bar bar{ts, Price::from_decimal(dec(b.open[r])), Price::from_decimal(dec(b.high[r])),
                Price::from_decimal(dec(b.low[r])), Price::from_decimal(dec(b.close[r])),
                Quantity::from_decimal(dec(b.volume[r]))};
        bar_storage[i].push_back(BarRow{universe[i], bar, ts, false});
      }
    }
    bar_spans.reserve(n);
    for (atx::usize i = 0; i < n; ++i) {
      bar_spans.emplace_back(bar_storage[i]);
    }
    if (!p.stats.empty()) {
      if (p.stats.size() != n) {
        throw std::runtime_error("stats must be empty or match symbols length");
      }
      stats = p.stats;
    }
    auto sc = Decimal::from_string(p.starting_cash);
    if (!sc) {
      throw std::runtime_error("starting_cash parse: " + sc.error().to_string());
    }
    starting_cash = *sc;
  }
};

// Collaborator layer: every non-Cap member the loop points at, constructed in
// declaration order over the already-built storage base.
struct BacktestEnv : BacktestStorage {
  SimClock clock;
  EventBus<> bus;
  InMemoryBarFeed feed;
  ScriptedSignalSource signal;
  WeightPolicy policy;
  atx::engine::exec::ExecutionSimulator exec;
  Portfolio portfolio;
  Market market;

  explicit BacktestEnv(const BacktestParams &p)
      : BacktestStorage(p), clock{}, bus{},
        feed{std::span<const std::span<const BarRow>>{bar_spans}, clock, bus},
        signal{p.signals, universe.size(), p.max_lookback}, policy{p.policy},
        exec{p.fill, p.slip, p.impact, p.comm, p.latency, p.volcap},
        portfolio{starting_cash, std::span<const InstrumentId>{universe}},
        market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}} {}
};

template <atx::usize Cap> struct BacktestRunner final : IBacktestRunner {
  BacktestEnv env;
  atx::engine::RollingPanel<Cap> panel;
  atx::engine::BacktestLoop<Cap> loop;

  explicit BacktestRunner(const BacktestParams &p)
      : env{p}, panel{std::span<const InstrumentId>{env.universe}, p.max_lookback},
        loop{env.feed,      env.clock,  env.bus,        panel,
             env.signal,    env.policy, env.exec,       env.portfolio,
             env.market,    Universe{env.universe},     Schedule{p.every},
             p.delay_same ? Delay::Same : Delay::Next} {}

  [[nodiscard]] atx::engine::BacktestResult run() override { return loop.run(); }
};

} // namespace

std::unique_ptr<IBacktestRunner> make_runner(const BacktestParams &p) {
  const atx::usize need = p.max_lookback;
  if (need == 0) {
    throw std::runtime_error("max_lookback must be >= 1");
  }
  if (need <= 8) return std::make_unique<BacktestRunner<8>>(p);
  if (need <= 16) return std::make_unique<BacktestRunner<16>>(p);
  if (need <= 32) return std::make_unique<BacktestRunner<32>>(p);
  if (need <= 64) return std::make_unique<BacktestRunner<64>>(p);
  if (need <= 128) return std::make_unique<BacktestRunner<128>>(p);
  if (need <= 256) return std::make_unique<BacktestRunner<256>>(p);
  if (need <= 512) return std::make_unique<BacktestRunner<512>>(p);
  if (need <= 1024) return std::make_unique<BacktestRunner<1024>>(p);
  if (need <= 4096) return std::make_unique<BacktestRunner<4096>>(p);
  throw std::runtime_error("max_lookback exceeds largest precompiled Cap (4096)");
}

} // namespace atxpy
```

> Implementation-time checks (grep the headers, fix qualifiers): exact namespaces of `InMemoryBarFeed`/`BarRow` (`atx::engine::data`), `InstrumentStats`/`Market`/`InstrumentId` (`atx::engine::loop` vs `atx::engine`), `ScriptedSignalSource` (`atx::engine`). The `using` block centralizes every such name — fix it once.

- [ ] **Step 3: Add both shim TUs to `python/CMakeLists.txt`** — add `src/_bindings/shim/backtest_shim.cpp` to the module sources and `target_include_directories(_core PRIVATE src/_bindings src/_bindings/shim)`.

- [ ] **Step 4: Build** — `pip install -e ./python --no-build-isolation`. Expected: compiles & links (no Python surface change yet). Fix qualifier/namespace errors surfaced by the compiler here. Commit after Task 3 (the shim is exercised there).

---

## Task 3: Bind `run_backtest` + result types (NumPy out)

**Files:** Create `python/src/_bindings/bind_backtest.cpp`; Modify `bind_core.cpp`, `python/CMakeLists.txt`; Test `python/tests/test_backtest_core.py`.

- [ ] **Step 1: Failing test** — `python/tests/test_backtest_core.py`:
```python
import numpy as np

from atxpy import _core


def _one_symbol_params(n=5):
    p = _core.BacktestParams()
    p.symbols = ["AAA"]
    bars = _core.BarsForSymbol()
    ts = [int(i) * 86_400_000_000_000 for i in range(n)]  # daily nanos
    bars.ts_nanos = ts
    bars.open = [100.0] * n
    bars.high = [101.0] * n
    bars.low = [99.0] * n
    bars.close = [100.0 + i for i in range(n)]
    bars.volume = [1_000_000.0] * n
    p.bars = [bars]
    p.starting_cash = "1000000.0"
    p.signals = [[1.0]] * n            # always long the one name
    p.max_lookback = 1
    p.every = 1
    return p


def test_run_returns_result_shape():
    p = _one_symbol_params(5)
    r = _core.run_backtest(p)
    assert r.slices == 5
    assert r.rebalances == 5
    ts, eq, gross, net = r.equity_columns()
    assert isinstance(eq, np.ndarray)
    assert eq.shape == (5,)
    assert ts.dtype == np.int64
    assert eq.dtype == np.float64
    # equity is finite and positive
    assert np.all(np.isfinite(eq))
    assert eq[0] > 0.0


def test_fills_columns_present():
    p = _one_symbol_params(5)
    r = _core.run_backtest(p)
    fid, fqty, fprice, ffee, fimpact, ft = r.fills_columns()
    assert fid.dtype == np.int64       # symbol id (column index)
    assert fqty.dtype == np.int64
    assert fprice.dtype == np.float64
    # single-name long: at least one buy fill
    assert fqty.size >= 1
    assert float(r.final_cash) <= 1_000_000.0
```

- [ ] **Step 2: Run, verify fail** — FAIL (`run_backtest`/`BacktestParams` missing).

- [ ] **Step 3: Write `python/src/_bindings/bind_backtest.cpp`**
```cpp
#include <cstdint>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/core/decimal.hpp"
#include "atx/engine/loop/backtest_loop.hpp"
#include "shim/backtest_shim.hpp"

namespace py = pybind11;
using atx::engine::BacktestResult;
using atx::engine::EquitySample;
using atx::engine::exec::FillPayload;

namespace {

// Owned NumPy column from a vector via a transform over the result rows.
template <class T, class Row, class Fn>
py::array_t<T> column(const std::vector<Row> &rows, Fn get) {
  py::array_t<T> out(static_cast<py::ssize_t>(rows.size()));
  auto v = out.mutable_unchecked();
  for (py::ssize_t i = 0; i < v.shape(0); ++i) {
    v(i) = get(rows[static_cast<std::size_t>(i)]);
  }
  return out;
}

void bind_backtest(py::module_ &m) {
  py::class_<atxpy::BarsForSymbol>(m, "BarsForSymbol")
      .def(py::init<>())
      .def_readwrite("ts_nanos", &atxpy::BarsForSymbol::ts_nanos)
      .def_readwrite("open", &atxpy::BarsForSymbol::open)
      .def_readwrite("high", &atxpy::BarsForSymbol::high)
      .def_readwrite("low", &atxpy::BarsForSymbol::low)
      .def_readwrite("close", &atxpy::BarsForSymbol::close)
      .def_readwrite("volume", &atxpy::BarsForSymbol::volume);

  py::class_<atxpy::BacktestParams>(m, "BacktestParams")
      .def(py::init<>())
      .def_readwrite("symbols", &atxpy::BacktestParams::symbols)
      .def_readwrite("bars", &atxpy::BacktestParams::bars)
      .def_readwrite("starting_cash", &atxpy::BacktestParams::starting_cash)
      .def_readwrite("signals", &atxpy::BacktestParams::signals)
      .def_readwrite("max_lookback", &atxpy::BacktestParams::max_lookback)
      .def_readwrite("every", &atxpy::BacktestParams::every)
      .def_readwrite("delay_same", &atxpy::BacktestParams::delay_same)
      .def_readwrite("policy", &atxpy::BacktestParams::policy)
      .def_readwrite("fill", &atxpy::BacktestParams::fill)
      .def_readwrite("slip", &atxpy::BacktestParams::slip)
      .def_readwrite("impact", &atxpy::BacktestParams::impact)
      .def_readwrite("comm", &atxpy::BacktestParams::comm)
      .def_readwrite("latency", &atxpy::BacktestParams::latency)
      .def_readwrite("volcap", &atxpy::BacktestParams::volcap)
      .def_readwrite("stats", &atxpy::BacktestParams::stats);

  py::class_<BacktestResult>(m, "BacktestResult")
      .def_property_readonly("slices", [](const BacktestResult &r) { return r.slices; })
      .def_property_readonly("rebalances", [](const BacktestResult &r) { return r.rebalances; })
      .def_property_readonly("turnover", [](const BacktestResult &r) { return r.turnover; })
      .def_property_readonly("final_equity", [](const BacktestResult &r) { return r.final_equity; })
      .def_property_readonly("final_cash",
                             [](const BacktestResult &r) { return r.final_cash; })
      .def("equity_columns",
           [](const BacktestResult &r) {
             return py::make_tuple(
                 column<std::int64_t>(r.equity_curve, [](const EquitySample &s) { return s.t.unix_nanos(); }),
                 column<double>(r.equity_curve, [](const EquitySample &s) { return s.equity; }),
                 column<double>(r.equity_curve, [](const EquitySample &s) { return s.gross; }),
                 column<double>(r.equity_curve, [](const EquitySample &s) { return s.net; }));
           })
      .def("fills_columns", [](const BacktestResult &r) {
        return py::make_tuple(
            column<std::int64_t>(r.fills, [](const FillPayload &f) { return static_cast<std::int64_t>(f.id.id); }),
            column<std::int64_t>(r.fills, [](const FillPayload &f) { return f.qty; }),
            column<double>(r.fills, [](const FillPayload &f) { return f.price.to_double(); }),
            column<double>(r.fills, [](const FillPayload &f) { return f.fee.to_double(); }),
            column<double>(r.fills, [](const FillPayload &f) { return f.impact; }),
            column<std::int64_t>(r.fills, [](const FillPayload &f) { return f.t.unix_nanos(); }));
      });

  m.def("run_backtest", [](const atxpy::BacktestParams &p) {
    auto runner = atxpy::make_runner(p);
    return runner->run();
  }, py::arg("params"), "Run a deterministic backtest; returns a BacktestResult.");
}

} // namespace

void bind_backtest_module(py::module_ &m) { bind_backtest(m); }
```

> `final_cash` returns an `atx::core::Decimal` — already bound in P0, so `float(r.final_cash)` works. `py::make_tuple` of arrays is fine.

- [ ] **Step 4: Wire into `bind_core.cpp`** — add `void bind_backtest_module(py::module_ &m);` decl and call `bind_backtest_module(m);` in `bind_core` (after `bind_config(m)`).

- [ ] **Step 5: Add TU to `python/CMakeLists.txt`** — append `src/_bindings/bind_backtest.cpp`.

- [ ] **Step 6: Build + test** — `pip install -e ./python --no-build-isolation`; `python -m pytest python/tests/test_backtest_core.py -v` → PASS.

- [ ] **Step 7: Commit** — `feat(atxpy): BacktestShim + run_backtest with NumPy result columns`.

---

## Task 4: Determinism + no-look-ahead parity

**Files:** Test `python/tests/test_backtest_determinism.py`.

- [ ] **Step 1: Write the test**
```python
import numpy as np

from atxpy import _core
from tests.test_backtest_core import _one_symbol_params  # reuse builder


def test_byte_identical_repeat():
    p = _one_symbol_params(10)
    a = _core.run_backtest(p)
    b = _core.run_backtest(p)
    ta, ea, ga, na = a.equity_columns()
    tb, eb, gb, nb = b.equity_columns()
    assert np.array_equal(ta, tb)
    assert np.array_equal(ea, eb)         # bit-identical equity
    assert a.final_cash.raw() == b.final_cash.raw()
    fa = a.fills_columns()
    fb = b.fills_columns()
    for ca, cb in zip(fa, fb):
        assert np.array_equal(ca, cb)


def test_no_look_ahead_prefix():
    # Truncating the feed after slice t leaves equity at <= t byte-identical.
    full = _one_symbol_params(10)
    short = _one_symbol_params(6)
    rf = _core.run_backtest(full)
    rs = _core.run_backtest(short)
    _, ef, _, _ = rf.equity_columns()
    _, es, _, _ = rs.equity_columns()
    assert es.shape == (6,)
    assert np.array_equal(ef[:6], es)     # the future is invisible to the past
```

> `_one_symbol_params` lives in `test_backtest_core.py`; ensure `python/tests` is importable as a package root (pytest adds rootdir). If the cross-import is fragile, move the builder into a `conftest.py` fixture instead.

- [ ] **Step 2: Run** — `python -m pytest python/tests/test_backtest_determinism.py -v` → PASS.

- [ ] **Step 3: Commit** — `test(atxpy): backtest determinism + no-look-ahead parity`.

---

## Task 5: pandas facade

**Files:** Create `python/src/atxpy/backtest.py`; Modify `python/src/atxpy/__init__.py`; Test `python/tests/test_backtest_facade.py`.

- [ ] **Step 1: Failing test** — `python/tests/test_backtest_facade.py`:
```python
import numpy as np
import pandas as pd

import atxpy


def _bars_df(symbols, n=5):
    rows = []
    for s_idx, sym in enumerate(symbols):
        for i in range(n):
            rows.append(
                dict(symbol=sym, timestamp=i * 86_400_000_000_000,
                     open=100.0, high=101.0, low=99.0,
                     close=100.0 + i + s_idx, volume=1_000_000.0)
            )
    return pd.DataFrame(rows)


def test_facade_runs_and_returns_frames():
    syms = ["AAA", "BBB"]
    bars = _bars_df(syms, 5)
    signals = np.tile([1.0, -1.0], (5, 1))  # long AAA / short BBB each rebalance
    res = atxpy.run_backtest(bars, signals, symbols=syms, starting_cash="1000000")
    assert isinstance(res.equity_curve, pd.DataFrame)
    assert list(res.equity_curve.columns) == ["timestamp", "equity", "gross", "net"]
    assert len(res.equity_curve) == 5
    assert isinstance(res.fills, pd.DataFrame)
    assert {"symbol", "qty", "price", "fee", "impact", "timestamp"} <= set(res.fills.columns)
    assert res.slices == 5
```

- [ ] **Step 2: Run, verify fail** — FAIL (`atxpy.run_backtest` missing).

- [ ] **Step 3: Write `python/src/atxpy/backtest.py`**
```python
"""Pythonic facade over atxpy._core.run_backtest (pandas in / pandas out)."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from . import _core


@dataclass
class BacktestResult:
    """Result of a backtest, with pandas views over the engine output."""

    equity_curve: "object"   # pd.DataFrame [timestamp, equity, gross, net]
    fills: "object"          # pd.DataFrame [symbol, qty, price, fee, impact, timestamp]
    final_cash: _core.Decimal
    final_equity: float
    turnover: float
    slices: int
    rebalances: int


def _require_pandas():
    try:
        import pandas as pd
    except ImportError as exc:  # pragma: no cover
        raise ImportError("atxpy.run_backtest needs pandas: pip install atxpy[pandas]") from exc
    return pd


def run_backtest(bars, signals, *, symbols=None, starting_cash="1000000.0",
                 max_lookback=1, every=1, delay_same=False, policy=None,
                 fill=None, slip=None, impact=None, comm=None, latency=None,
                 volcap=None, stats=None):
    """Run a backtest from a long-format bars DataFrame and a signals matrix.

    bars: DataFrame with columns [symbol, timestamp, open, high, low, close, volume];
          timestamp is int64 unix-nanos. Rows are grouped by symbol and sorted by
          timestamp to form the per-symbol feed.
    signals: 2D array-like [n_rebalances, n_symbols] of alpha scores, column-aligned
             to `symbols` (NaN = no opinion). Row k is used on the k-th rebalance.
    symbols: explicit universe order; defaults to sorted(unique(bars.symbol)).
    """
    pd = _require_pandas()
    if symbols is None:
        symbols = sorted(bars["symbol"].unique())
    symbols = list(symbols)

    p = _core.BacktestParams()
    p.symbols = symbols
    bars_list = []
    for sym in symbols:
        g = bars[bars["symbol"] == sym].sort_values("timestamp")
        b = _core.BarsForSymbol()
        b.ts_nanos = g["timestamp"].to_numpy(dtype=np.int64).tolist()
        b.open = g["open"].to_numpy(dtype=np.float64).tolist()
        b.high = g["high"].to_numpy(dtype=np.float64).tolist()
        b.low = g["low"].to_numpy(dtype=np.float64).tolist()
        b.close = g["close"].to_numpy(dtype=np.float64).tolist()
        b.volume = g["volume"].to_numpy(dtype=np.float64).tolist()
        bars_list.append(b)
    p.bars = bars_list
    p.starting_cash = str(starting_cash)
    sig = np.asarray(signals, dtype=np.float64)
    if sig.ndim != 2:
        raise ValueError("signals must be 2D [n_rebalances, n_symbols]")
    if sig.shape[1] != len(symbols):
        raise ValueError(f"signals has {sig.shape[1]} cols, expected {len(symbols)}")
    p.signals = [row.tolist() for row in sig]
    p.max_lookback = int(max_lookback)
    p.every = int(every)
    p.delay_same = bool(delay_same)
    if policy is not None:
        p.policy = policy
    for name, cfg in dict(fill=fill, slip=slip, impact=impact, comm=comm,
                          latency=latency, volcap=volcap).items():
        if cfg is not None:
            setattr(p, name, cfg)
    if stats is not None:
        p.stats = stats

    r = _core.run_backtest(p)

    ts, eq, gross, net = r.equity_columns()
    equity_curve = pd.DataFrame({"timestamp": ts, "equity": eq, "gross": gross, "net": net})
    fid, fqty, fprice, ffee, fimpact, ft = r.fills_columns()
    name_by_id = {i: s for i, s in enumerate(symbols)}
    fills = pd.DataFrame({
        "symbol": [name_by_id.get(int(i), int(i)) for i in fid],
        "qty": fqty, "price": fprice, "fee": ffee, "impact": fimpact, "timestamp": ft,
    })
    return BacktestResult(
        equity_curve=equity_curve, fills=fills, final_cash=r.final_cash,
        final_equity=r.final_equity, turnover=r.turnover, slices=r.slices,
        rebalances=r.rebalances,
    )
```

- [ ] **Step 4: Re-export in `__init__.py`** — add:
```python
from .backtest import BacktestResult, run_backtest
```
and add `"BacktestResult"`, `"run_backtest"` to `__all__`. Also re-export the config types from `_core` for convenience: `WeightPolicy`, `Transform`, `SlippageCfg`, `CommissionCfg`, `ImpactCfg`, `FillCfg`, `LatencyCfg`, `VolumeCapCfg`, `InstrumentStats`, `BacktestParams`, `BarsForSymbol`.

- [ ] **Step 5: Run** — `python -m pytest python/tests/test_backtest_facade.py -v` → PASS. (No rebuild needed — pure-Python change.)

- [ ] **Step 6: Commit** — `feat(atxpy): pandas facade for run_backtest`.

---

## Task 6: Stubs + full P1 suite + close

**Files:** Modify `python/src/atxpy/_core.pyi`; Test: run everything.

- [ ] **Step 1: Extend `_core.pyi`** with the new bound types (enums `Transform`/`SlippageMode`/`CommissionMode`/`OrderType`; the cfg classes; `InstrumentStats`; `WeightPolicy`; `BarsForSymbol`; `BacktestParams`; `BacktestResult` with `equity_columns()`/`fills_columns()` returning tuples of `np.ndarray`; `run_backtest`). Mirror the C++ readwrite fields as annotated attributes.

- [ ] **Step 2: Full suite** — `python -m pytest python/tests -v` → all green (P0 + P1).

- [ ] **Step 3: Commit** — `docs(atxpy): P1 type stubs; close backtest vertical`.

---

## Self-Review

- **Spec coverage (design §9 P1):** InMemoryBarFeed (bars→DataFrame) ✔, BacktestShim w/ Cap dispatch + owned graph ✔, ScriptedSignalSource ✔, ExecutionSimulator + all 6 cost cfgs ✔, Portfolio ✔, WeightPolicy ✔, Market ✔, BacktestResult→DataFrame ✔, determinism parity ✔ (Task 4).
- **Lifetime safety:** the shim owns the entire graph (`BacktestStorage` base built first → collaborators borrow stable storage → `RollingPanel`/`loop` last); nothing non-owning crosses into Python; result is returned by value. No `py::keep_alive` needed here because Python never holds a borrowed sub-object.
- **No placeholders:** every TU's code is given. Two implementation-time TODOs are explicit and bounded: (a) confirm `InstrumentStats`/`Market` namespace qualifier, (b) the test cross-import vs conftest fixture — both have a stated fallback.
- **Type consistency:** `bind_backtest_module` (declared in bind_core.cpp) wraps `bind_backtest` (file-local). `make_runner`/`IBacktestRunner`/`BacktestParams`/`BarsForSymbol` names match across hpp/cpp/bindings. NumPy column dtypes (`int64` ts/id/qty, `float64` prices/equity) match the test assertions.
- **Determinism:** no RNG added; the engine's fixed-index reductions and FIFO settle are untouched; the shim only marshals data in and a value result out.

## Open follow-ups (post-P1)
- Real strategy via the alpha DSL (P2) replaces `ScriptedSignalSource` with `VmSignalSource`.
- Zero-copy bars in (accept NumPy arrays directly into `BarsForSymbol` instead of `.tolist()`) — perf, once correctness is locked.
- Per-period turnover ratio + drawdown helpers on the facade result.
