#include <optional>
#include <span>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/core/types.hpp"
#include "atx/engine/eval/cpcv.hpp"
#include "atx/engine/eval/deflated_sharpe.hpp"
#include "atx/engine/eval/pbo.hpp"
#include "atx/engine/eval/perf_metrics.hpp"
#include "atx/engine/eval/stats_ext.hpp"
#include "result.hpp"

namespace py = pybind11;
namespace ev = atx::engine::eval;

namespace {

template <class T> std::span<const T> as_span(const std::vector<T> &v) {
  return std::span<const T>{v.data(), v.size()};
}

void bind_eval(py::module_ &m) {
  // ---- performance metrics ----
  py::class_<ev::ReturnMetrics>(m, "ReturnMetrics", "Performance summary of a return series.")
      .def_readonly("sharpe", &ev::ReturnMetrics::sharpe)
      .def_readonly("sortino", &ev::ReturnMetrics::sortino)
      .def_readonly("max_dd", &ev::ReturnMetrics::max_dd)
      .def_readonly("calmar", &ev::ReturnMetrics::calmar)
      .def_readonly("ir", &ev::ReturnMetrics::ir)
      .def_readonly("appraisal", &ev::ReturnMetrics::appraisal)
      .def_readonly("hit_rate", &ev::ReturnMetrics::hit_rate);

  m.def(
      "compute_return_metrics",
      [](const std::vector<double> &pnl, double periods_per_year) {
        ev::ReturnMetricsCfg cfg;
        cfg.periods_per_year = periods_per_year;
        return ev::compute_return_metrics(as_span(pnl), cfg);
      },
      py::arg("pnl"), py::arg("periods_per_year") = 252.0,
      "Sharpe/Sortino/max-drawdown/Calmar/hit-rate from a per-period return series.");

  // ---- deflated / probabilistic Sharpe ----
  py::class_<ev::DsrResult>(m, "DsrResult", "Deflated Sharpe ratio result.")
      .def_readonly("psr", &ev::DsrResult::psr)
      .def_readonly("sr_star", &ev::DsrResult::sr_star)
      .def_readonly("dsr", &ev::DsrResult::dsr)
      .def_readonly("haircut_sharpe", &ev::DsrResult::haircut_sharpe);

  m.def(
      "deflated_sharpe",
      [](double sr, atx::usize T, double skew, double exkurt, atx::usize N,
         std::optional<double> var) { return ev::deflated_sharpe(sr, T, skew, exkurt, N, var); },
      py::arg("sr"), py::arg("n_obs"), py::arg("skew") = 0.0, py::arg("excess_kurtosis") = 0.0,
      py::arg("n_trials") = 1, py::arg("variance") = std::nullopt,
      "Deflated Sharpe ratio across n_trials candidate strategies.");

  // ---- probability of backtest overfitting ----
  py::class_<ev::PboResult>(m, "PboResult", "Probability of backtest overfitting (CSCV).")
      .def_readonly("pbo", &ev::PboResult::pbo)
      .def_readonly("split_logits", &ev::PboResult::split_logits)
      .def_readonly("mean_logit", &ev::PboResult::mean_logit);

  m.def(
      "pbo_cscv",
      [](const std::vector<double> &perf, atx::usize n_candidates, atx::usize n_splits) {
        return atxpy::unwrap(ev::pbo_cscv_checked(as_span(perf), n_candidates, n_splits));
      },
      py::arg("perf"), py::arg("n_candidates"), py::arg("n_splits"),
      "CSCV PBO over candidate-major flattened performance (len = n_candidates * T).");

  // ---- combinatorial purged CV folds ----
  py::class_<ev::LabelSpan>(m, "LabelSpan", "Half-open [t0, t1) label information window.")
      .def(py::init([](atx::usize t0, atx::usize t1) { return ev::LabelSpan{t0, t1}; }),
           py::arg("t0"), py::arg("t1"))
      .def_readwrite("t0", &ev::LabelSpan::t0)
      .def_readwrite("t1", &ev::LabelSpan::t1);
  py::class_<ev::CpcvConfig>(m, "CpcvConfig")
      .def(py::init<>())
      .def_readwrite("n_groups", &ev::CpcvConfig::n_groups)
      .def_readwrite("n_test_groups", &ev::CpcvConfig::n_test_groups)
      .def_readwrite("embargo", &ev::CpcvConfig::embargo);
  py::class_<ev::CpcvFold>(m, "CpcvFold")
      .def_readonly("train_idx", &ev::CpcvFold::train_idx)
      .def_readonly("test_idx", &ev::CpcvFold::test_idx);
  m.def(
      "cpcv_folds",
      [](const std::vector<ev::LabelSpan> &spans, const ev::CpcvConfig &cfg) {
        return ev::cpcv_folds(as_span(spans), cfg);
      },
      py::arg("spans"), py::arg("config"),
      "Generate C(K,k) combinatorial purged CV folds from per-observation label spans.");

  // ---- standalone stats ----
  py::class_<ev::MeanStd>(m, "MeanStd").def_readonly("mean", &ev::MeanStd::mean).def_readonly(
      "std", &ev::MeanStd::std);
  m.def("mean_std_pop", [](const std::vector<double> &r) { return ev::mean_std_pop(as_span(r)); },
        py::arg("series"));
  m.def("skewness", [](const std::vector<double> &r) { return ev::skewness(as_span(r)); },
        py::arg("series"));
  m.def("excess_kurtosis",
        [](const std::vector<double> &r) { return ev::excess_kurtosis(as_span(r)); },
        py::arg("series"));
  m.def("median", [](const std::vector<double> &r) { return ev::median(as_span(r)); },
        py::arg("series"));
  m.def("norm_cdf", &ev::norm_cdf, py::arg("x"));
  m.def("norm_ppf", &ev::norm_ppf, py::arg("p"));
  m.def("returns_from_equity",
        [](const std::vector<double> &equity) { return ev::returns_from_equity(as_span(equity)); },
        py::arg("equity"), "Simple per-period returns from an equity-level series.");
}

} // namespace

void bind_eval_module(py::module_ &m) { bind_eval(m); }
