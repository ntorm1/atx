#include <cstddef>
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

// Owned NumPy column from a vector of rows via a per-row accessor.
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
      .def_property_readonly("final_cash", [](const BacktestResult &r) { return r.final_cash; })
      .def(
          "equity_columns",
          [](const BacktestResult &r) {
            return py::make_tuple(
                column<std::int64_t>(r.equity_curve,
                                     [](const EquitySample &s) { return s.t.unix_nanos(); }),
                column<double>(r.equity_curve, [](const EquitySample &s) { return s.equity; }),
                column<double>(r.equity_curve, [](const EquitySample &s) { return s.gross; }),
                column<double>(r.equity_curve, [](const EquitySample &s) { return s.net; }));
          },
          "Return (ts_nanos[int64], equity[f64], gross[f64], net[f64]) NumPy arrays.")
      .def(
          "fills_columns",
          [](const BacktestResult &r) {
            return py::make_tuple(
                column<std::int64_t>(
                    r.fills, [](const FillPayload &f) { return static_cast<std::int64_t>(f.id.id); }),
                column<std::int64_t>(r.fills, [](const FillPayload &f) { return f.qty; }),
                column<double>(r.fills, [](const FillPayload &f) { return f.price.to_double(); }),
                column<double>(r.fills, [](const FillPayload &f) { return f.fee.to_double(); }),
                column<double>(r.fills, [](const FillPayload &f) { return f.impact; }),
                column<std::int64_t>(r.fills,
                                     [](const FillPayload &f) { return f.t.unix_nanos(); }));
          },
          "Return (sym_id[int64], qty[int64], price[f64], fee[f64], impact[f64], ts_nanos[int64]).");

  m.def(
      "run_backtest",
      [](const atxpy::BacktestParams &p) {
        auto runner = atxpy::make_runner(p);
        return runner->run();
      },
      py::arg("params"), "Run a deterministic backtest; returns a BacktestResult.");
}

} // namespace

void bind_backtest_module(py::module_ &m) { bind_backtest(m); }
