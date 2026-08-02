// Bindings for the backtest analytics layer: the TearSheet fold and the two
// deterministic TSV exports.
//
// `write_pnl_tsv` accepts the metadata header as either a dict or a sequence of
// (key, value) pairs — both preserve insertion order, which the renderer relies
// on when it echoes the header back into the chart's stats box.

#include <span>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/backtest.hpp"
#include "atx/vol/tools/tearsheet.hpp"
#include "backtest_util.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

std::vector<std::pair<std::string, std::string>> as_meta(const py::object &meta) {
  std::vector<std::pair<std::string, std::string>> out;
  if (py::isinstance<py::dict>(meta)) {
    const auto items = py::cast<py::dict>(meta);
    out.reserve(items.size());
    for (const auto &[key, value] : items) {
      out.emplace_back(py::str(key), py::str(value));
    }
    return out;
  }
  const auto items = py::cast<py::sequence>(meta);
  out.reserve(items.size());
  for (const auto handle : items) {
    auto entry = py::cast<py::sequence>(handle);
    if (entry.size() != 2) {
      throw py::value_error("each meta item must be a (key, value) pair");
    }
    out.emplace_back(py::str(entry[0]), py::str(entry[1]));
  }
  return out;
}

} // namespace

void bind_analytics(py::module_ &m) {
  py::class_<TearSheet>(m, "TearSheet")
      .def(py::init<>())
      .def_readonly("total_return", &TearSheet::total_return)
      .def_readonly("ann_return", &TearSheet::ann_return)
      .def_readonly("ann_vol", &TearSheet::ann_vol)
      .def_readonly("sharpe", &TearSheet::sharpe)
      .def_readonly("max_drawdown", &TearSheet::max_drawdown)
      .def_readonly("hit_rate", &TearSheet::hit_rate)
      .def_readonly("avg_turnover", &TearSheet::avg_turnover)
      .def_readonly("total_cost", &TearSheet::total_cost)
      .def_readonly("total_financing", &TearSheet::total_financing)
      .def_readonly("attr_delta", &TearSheet::attr_delta)
      .def_readonly("attr_gamma", &TearSheet::attr_gamma)
      .def_readonly("attr_vega", &TearSheet::attr_vega)
      .def_readonly("attr_vanna", &TearSheet::attr_vanna)
      .def_readonly("attr_volga", &TearSheet::attr_volga)
      .def_readonly("attr_theta", &TearSheet::attr_theta)
      .def_readonly("attr_rho", &TearSheet::attr_rho)
      .def_readonly("attr_charm", &TearSheet::attr_charm)
      .def_readonly("attr_unexplained", &TearSheet::attr_unexplained)
      .def_readonly("attr_settlement", &TearSheet::attr_settlement)
      .def_readonly("attr_shares", &TearSheet::attr_shares)
      .def_readonly("attr_financing", &TearSheet::attr_financing)
      .def_readonly("attr_cost", &TearSheet::attr_cost)
      .def_readonly("return_on_gross_vega", &TearSheet::return_on_gross_vega)
      .def_readonly("vega_adj_sharpe", &TearSheet::vega_adj_sharpe)
      .def_readonly("pnl_per_vega_traded", &TearSheet::pnl_per_vega_traded)
      .def_readonly("avg_gross_vega", &TearSheet::avg_gross_vega)
      .def_readonly("avg_gross_gamma", &TearSheet::avg_gross_gamma)
      .def("to_dict", [](const TearSheet &t) {
        py::dict d;
#define ATXVOL_FIELD(name) d[#name] = t.name
        ATXVOL_FIELD(total_return);
        ATXVOL_FIELD(ann_return);
        ATXVOL_FIELD(ann_vol);
        ATXVOL_FIELD(sharpe);
        ATXVOL_FIELD(max_drawdown);
        ATXVOL_FIELD(hit_rate);
        ATXVOL_FIELD(avg_turnover);
        ATXVOL_FIELD(total_cost);
        ATXVOL_FIELD(total_financing);
        ATXVOL_FIELD(attr_delta);
        ATXVOL_FIELD(attr_gamma);
        ATXVOL_FIELD(attr_vega);
        ATXVOL_FIELD(attr_vanna);
        ATXVOL_FIELD(attr_volga);
        ATXVOL_FIELD(attr_theta);
        ATXVOL_FIELD(attr_rho);
        ATXVOL_FIELD(attr_charm);
        ATXVOL_FIELD(attr_unexplained);
        ATXVOL_FIELD(attr_settlement);
        ATXVOL_FIELD(attr_shares);
        ATXVOL_FIELD(attr_financing);
        ATXVOL_FIELD(attr_cost);
        ATXVOL_FIELD(return_on_gross_vega);
        ATXVOL_FIELD(vega_adj_sharpe);
        ATXVOL_FIELD(pnl_per_vega_traded);
        ATXVOL_FIELD(avg_gross_vega);
        ATXVOL_FIELD(avg_gross_gamma);
#undef ATXVOL_FIELD
        return d;
      });

  // Every consumer below indexes all columns by the row count, so a ragged
  // hand-built result would read out of bounds. Guard before the C++ call.
  m.def(
      "tearsheet",
      [](const BacktestResult &r, double periods_per_year) {
        atxvol::python::require_consistent(r, "tearsheet");
        py::gil_scoped_release release;
        return tearsheet(r, periods_per_year);
      },
      py::arg("result"), py::arg("periods_per_year") = 252.0,
      "Fold a BacktestResult into headline + attribution + vega-scaled metrics.");

  m.def(
      "write_backtest_tsv",
      [](const BacktestResult &r, const std::string &path) {
        atxvol::python::require_consistent(r, "write_backtest_tsv");
        py::gil_scoped_release release;
        atxvol::python::unwrap(write_backtest_tsv(r, path));
      },
      py::arg("result"), py::arg("path"),
      "Write every column as a deterministic TSV (%.17g, bit-exact round-trip).");

  m.def(
      "write_backtest_pnl_tsv",
      [](const BacktestResult &r, const py::object &meta, const std::string &path) {
        atxvol::python::require_consistent(r, "write_backtest_pnl_tsv");
        const auto pairs = as_meta(meta);
        py::gil_scoped_release release;
        atxvol::python::unwrap(write_backtest_pnl_tsv(
            r, std::span<const std::pair<std::string, std::string>>{pairs}, path));
      },
      py::arg("result"), py::arg("meta"), py::arg("path"),
      "Write the PnL-track TSV: a `# key=value` meta header then the series. "
      "`meta` may be a dict or a sequence of (key, value) pairs.");
}
