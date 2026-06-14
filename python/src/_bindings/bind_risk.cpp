#include <span>
#include <vector>

#include <pybind11/eigen.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/optimizer.hpp"
#include "result.hpp"

namespace py = pybind11;
namespace rk = atx::engine::risk;

namespace {

std::span<const double> as_span(const std::vector<double> &v) {
  return std::span<const double>{v.data(), v.size()};
}

void bind_risk(py::module_ &m) {
  py::class_<rk::FactorModel>(m, "FactorModel",
                              "Barra-style factor risk model V = X F X^T + diag(D).")
      .def_property_readonly("n_factors", &rk::FactorModel::n_factors)
      .def_property_readonly("n_instruments", &rk::FactorModel::n_instruments)
      .def("risk", [](const rk::FactorModel &fm,
                      const std::vector<double> &w) { return fm.risk(as_span(w)); },
           py::arg("weights"), "Portfolio variance w^T V w.")
      .def(
          "neutralize",
          [](const rk::FactorModel &fm, std::vector<double> signal) {
            fm.neutralize(std::span<double>{signal.data(), signal.size()});
            return signal;
          },
          py::arg("signal"), "Return the factor-neutralized copy of a signal vector.");

  m.def(
      "make_factor_model",
      [](atx::core::linalg::MatX x, atx::core::linalg::MatX f, atx::core::linalg::VecX d,
         atx::usize fit_begin, atx::usize fit_end) {
        return atxpy::unwrap(rk::FactorModel::create(std::move(x), std::move(f), std::move(d),
                                                     fit_begin, fit_end));
      },
      py::arg("X"), py::arg("F"), py::arg("D"), py::arg("fit_begin") = 0, py::arg("fit_end") = 0,
      "Build a FactorModel from exposures X (M x K), factor cov F (K x K), specific var D (M).");

  py::class_<rk::OptimizerConfig>(m, "OptimizerConfig")
      .def(py::init<>())
      .def_readwrite("risk_aversion", &rk::OptimizerConfig::risk_aversion)
      .def_readwrite("turnover_penalty", &rk::OptimizerConfig::turnover_penalty)
      .def_readwrite("gross_leverage", &rk::OptimizerConfig::gross_leverage)
      .def_readwrite("name_cap", &rk::OptimizerConfig::name_cap)
      .def_readwrite("dollar_neutral", &rk::OptimizerConfig::dollar_neutral)
      .def_readwrite("max_iters", &rk::OptimizerConfig::max_iters);

  py::class_<rk::PortfolioOptimizer>(m, "PortfolioOptimizer",
                                     "Turnover-penalized mean-variance optimizer (fixed-iteration).")
      .def(py::init([](const rk::OptimizerConfig &cfg) { return rk::PortfolioOptimizer{cfg}; }),
           py::arg("config"))
      .def_readwrite("cfg", &rk::PortfolioOptimizer::cfg)
      .def(
          "solve",
          [](const rk::PortfolioOptimizer &opt, const std::vector<double> &alpha,
             const rk::FactorModel &model, const std::vector<double> &w_prev) {
            return atxpy::unwrap(opt.solve(as_span(alpha), model, as_span(w_prev)));
          },
          py::arg("alpha"), py::arg("model"), py::arg("w_prev") = std::vector<double>{},
          "Solve for target weights given an alpha vector, a FactorModel, and prior weights.");
}

} // namespace

void bind_risk_module(py::module_ &m) { bind_risk(m); }
