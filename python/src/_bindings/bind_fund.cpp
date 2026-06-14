#include <span>
#include <vector>

#include <pybind11/eigen.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/fund/cross_sleeve_risk.hpp"
#include "atx/engine/fund/meta_allocator.hpp"
#include "result.hpp"

namespace py = pybind11;
namespace fund = atx::engine::fund;

namespace {

void bind_fund(py::module_ &m) {
  py::enum_<fund::RiskBudgetMethod>(m, "RiskBudgetMethod")
      .value("InverseVol", fund::RiskBudgetMethod::InverseVol)
      .value("EqualRiskContribution", fund::RiskBudgetMethod::EqualRiskContribution)
      .value("HierarchicalRiskParity", fund::RiskBudgetMethod::HierarchicalRiskParity);

  py::class_<fund::MetaAllocatorConfig>(m, "MetaAllocatorConfig")
      .def(py::init<>())
      .def_readwrite("method", &fund::MetaAllocatorConfig::method)
      .def_readwrite("risk_budget", &fund::MetaAllocatorConfig::risk_budget)
      .def_readwrite("fractional_kelly", &fund::MetaAllocatorConfig::fractional_kelly)
      .def_readwrite("target_vol", &fund::MetaAllocatorConfig::target_vol)
      .def_readwrite("max_gross", &fund::MetaAllocatorConfig::max_gross)
      .def_readwrite("solve_iters", &fund::MetaAllocatorConfig::solve_iters);

  py::class_<fund::CapitalWeights>(m, "CapitalWeights")
      .def_readonly("c", &fund::CapitalWeights::c);

  py::class_<fund::MetaAllocator>(m, "MetaAllocator",
                                  "Risk-budget + fractional-Kelly capital allocator across sleeves.")
      .def(py::init([](const fund::MetaAllocatorConfig &cfg) {
             fund::MetaAllocator a;
             a.cfg = cfg;
             return a;
           }),
           py::arg("config"))
      .def_readwrite("cfg", &fund::MetaAllocator::cfg)
      .def(
          "allocate",
          [](const fund::MetaAllocator &a, const atx::core::linalg::MatX &Omega,
             const std::vector<double> &sleeve_vol, const std::vector<double> &caps) {
            return atxpy::unwrap(a.allocate(
                Omega, std::span<const double>{sleeve_vol.data(), sleeve_vol.size()},
                std::span<const double>{caps.data(), caps.size()}));
          },
          py::arg("Omega"), py::arg("sleeve_vol"), py::arg("caps"),
          "Allocate per-sleeve capital from a sleeve-return covariance, vols, and capacity caps.");

  m.def(
      "sleeve_return_cov",
      [](const std::vector<std::vector<double>> &sleeve_pnl) {
        std::vector<std::span<const double>> spans;
        spans.reserve(sleeve_pnl.size());
        for (const std::vector<double> &row : sleeve_pnl) {
          spans.emplace_back(row.data(), row.size());
        }
        return atxpy::unwrap(fund::sleeve_return_cov(
            std::span<const std::span<const double>>{spans.data(), spans.size()}));
      },
      py::arg("sleeve_pnl"),
      "Build the S x S sleeve-return covariance Omega from per-sleeve PnL series.");
}

} // namespace

void bind_fund_module(py::module_ &m) { bind_fund(m); }
