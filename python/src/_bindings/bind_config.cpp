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
      .value("ZScore", e::Transform::ZScore)
      .value("Raw", e::Transform::Raw);
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

  py::class_<e::InstrumentStats>(m, "InstrumentStats")
      .def(py::init<>())
      .def_readwrite("adv", &e::InstrumentStats::adv)
      .def_readwrite("sigma", &e::InstrumentStats::sigma)
      .def_readwrite("spread", &e::InstrumentStats::spread);

  py::class_<e::WeightPolicy>(m, "WeightPolicy")
      .def(py::init<>())
      .def_readwrite("transform", &e::WeightPolicy::transform)
      .def_readwrite("industry_neutral", &e::WeightPolicy::industry_neutral)
      .def_readwrite("dollar_neutral", &e::WeightPolicy::dollar_neutral)
      .def_readwrite("gross_leverage", &e::WeightPolicy::gross_leverage)
      .def_readwrite("truncation", &e::WeightPolicy::truncation)
      .def_readwrite("winsorize_limit", &e::WeightPolicy::winsorize_limit);
}
