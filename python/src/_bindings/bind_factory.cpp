#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/core/types.hpp"
#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/factory/factory.hpp"
#include "shim/mining_shim.hpp"
#include "result.hpp"

namespace py = pybind11;
namespace factory = atx::engine::factory;
namespace combine = atx::engine::combine;

namespace {

py::array_t<double> span_to_array(std::span<const double> s) {
  py::array_t<double> out(static_cast<py::ssize_t>(s.size()));
  auto v = out.mutable_unchecked();
  for (py::ssize_t i = 0; i < v.shape(0); ++i) {
    v(i) = s[static_cast<std::size_t>(i)];
  }
  return out;
}

void bind_factory(py::module_ &m) {
  py::class_<combine::AlphaMetrics>(m, "AlphaMetrics", "Per-alpha performance summary.")
      .def_readonly("sharpe", &combine::AlphaMetrics::sharpe)
      .def_readonly("turnover", &combine::AlphaMetrics::turnover)
      .def_readonly("returns", &combine::AlphaMetrics::returns)
      .def_readonly("drawdown", &combine::AlphaMetrics::drawdown)
      .def_readonly("margin", &combine::AlphaMetrics::margin)
      .def_readonly("fitness", &combine::AlphaMetrics::fitness)
      .def_readonly("holding_days", &combine::AlphaMetrics::holding_days);

  py::class_<combine::AlphaStore>(m, "AlphaStore", "Admitted-alpha pool (pnl + metrics).")
      .def_property_readonly("n_alphas", &combine::AlphaStore::n_alphas)
      .def_property_readonly("n_periods", &combine::AlphaStore::n_periods)
      .def_property_readonly("n_instruments", &combine::AlphaStore::n_instruments)
      .def("pnl",
           [](const combine::AlphaStore &s, atx::usize a) {
             return span_to_array(s.pnl(combine::AlphaId{static_cast<atx::u32>(a)}));
           },
           py::arg("alpha_index"), "PnL stream (length n_periods) for an admitted alpha.")
      .def("metrics",
           [](const combine::AlphaStore &s, atx::usize a) {
             return s.get(combine::AlphaId{static_cast<atx::u32>(a)}).metrics;
           },
           py::arg("alpha_index"), "AlphaMetrics for an admitted alpha.");

  py::class_<factory::FactoryReport>(m, "FactoryReport", "Telemetry of an alpha-mining run.")
      .def_readonly("admitted", &factory::FactoryReport::admitted)
      .def_readonly("evaluated", &factory::FactoryReport::evaluated)
      .def_readonly("dedup_pct", &factory::FactoryReport::dedup_pct)
      .def_readonly("cse_pct", &factory::FactoryReport::cse_pct)
      .def_readonly("trials", &factory::FactoryReport::trials)
      .def_readonly("seed", &factory::FactoryReport::seed)
      .def_readonly("digest", &factory::FactoryReport::digest)
      .def_readonly("duplicates", &factory::FactoryReport::duplicates);

  py::class_<atxpy::MineParams>(m, "MineParams")
      .def(py::init<>())
      .def_readwrite("dates", &atxpy::MineParams::dates)
      .def_readwrite("instruments", &atxpy::MineParams::instruments)
      .def_readwrite("field_names", &atxpy::MineParams::field_names)
      .def_readwrite("field_columns", &atxpy::MineParams::field_columns)
      .def_readwrite("seed_exprs", &atxpy::MineParams::seed_exprs)
      .def_readwrite("panel_fields", &atxpy::MineParams::panel_fields)
      .def_readwrite("master_seed", &atxpy::MineParams::master_seed)
      .def_readwrite("population", &atxpy::MineParams::population)
      .def_readwrite("generations", &atxpy::MineParams::generations)
      .def_readwrite("elites", &atxpy::MineParams::elites)
      .def_readwrite("k_tournament", &atxpy::MineParams::k_tournament)
      .def_readwrite("p_cross", &atxpy::MineParams::p_cross)
      .def_readwrite("enable_behavioral_novelty", &atxpy::MineParams::enable_behavioral_novelty)
      .def_readwrite("max_lookback", &atxpy::MineParams::max_lookback)
      .def_readwrite("trial_count", &atxpy::MineParams::trial_count)
      .def_readwrite("min_dsr", &atxpy::MineParams::min_dsr)
      .def_readwrite("book_size", &atxpy::MineParams::book_size)
      .def_readwrite("min_sharpe", &atxpy::MineParams::min_sharpe)
      .def_readwrite("min_fitness", &atxpy::MineParams::min_fitness)
      .def_readwrite("max_turnover", &atxpy::MineParams::max_turnover)
      .def_readwrite("max_pool_corr", &atxpy::MineParams::max_pool_corr);

  py::class_<atxpy::MineResult>(m, "MineResult")
      .def_property_readonly("report",
                             [](const atxpy::MineResult &r) { return r.report; })
      .def_property_readonly(
          "pool", [](atxpy::MineResult &r) -> combine::AlphaStore & { return r.pool; },
          py::return_value_policy::reference_internal);

  m.def("mine_alphas", &atxpy::mine_alphas, py::arg("params"),
        "Run an evolutionary alpha-mining search; returns a MineResult (report + pool).");

  // ---- combine ----
  py::enum_<combine::CombineMethod>(m, "CombineMethod")
      .value("EqualWeight", combine::CombineMethod::EqualWeight)
      .value("RankAverage", combine::CombineMethod::RankAverage)
      .value("IcWeighted", combine::CombineMethod::IcWeighted)
      .value("ShrinkageMv", combine::CombineMethod::ShrinkageMv)
      .value("BoundedRegression", combine::CombineMethod::BoundedRegression);

  py::class_<combine::CombinerConfig>(m, "CombinerConfig")
      .def(py::init<>())
      .def_readwrite("method", &combine::CombinerConfig::method)
      .def_readwrite("shrinkage", &combine::CombinerConfig::shrinkage)
      .def_readwrite("weight_bound", &combine::CombinerConfig::weight_bound)
      .def_readwrite("ridge_lambda", &combine::CombinerConfig::ridge_lambda)
      .def_readwrite("n_pcs", &combine::CombinerConfig::n_pcs);

  py::class_<combine::Combination>(m, "Combination")
      .def_readonly("weights", &combine::Combination::weights)
      .def_readonly("fit_begin", &combine::Combination::fit_begin)
      .def_readonly("fit_end", &combine::Combination::fit_end);

  py::class_<combine::AlphaCombiner>(m, "AlphaCombiner", "Blends a pool of alphas into weights.")
      .def(py::init([](const combine::CombinerConfig &cfg) {
             combine::AlphaCombiner c;
             c.cfg = cfg;
             return c;
           }),
           py::arg("config"))
      .def_readwrite("cfg", &combine::AlphaCombiner::cfg)
      .def(
          "fit",
          [](const combine::AlphaCombiner &c, const combine::AlphaStore &pool, atx::usize fit_begin,
             atx::usize fit_end) { return atxpy::unwrap(c.fit(pool, fit_begin, fit_end)); },
          py::arg("pool"), py::arg("fit_begin"), py::arg("fit_end"),
          "Fit per-alpha blend weights over the [fit_begin, fit_end) period window.");
}

} // namespace

void bind_factory_module(py::module_ &m) { bind_factory(m); }
