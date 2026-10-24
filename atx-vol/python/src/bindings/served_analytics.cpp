// A1 served analytics (analytics/served_analytics.hpp, r6): hedge Greeks, scenario
// matrices and P&L attribution over a ServeBook, plus D1's dated-cash scenarios (lane PY,
// 2026-10-23).
//
// Every answer carries its AnalyticsDeclaration (model / version / hedge convention /
// clock) and a typed AnalyticsStatus; a refusal is a status with NaN numbers, never a
// silent value. Scenario cells come back as numpy arrays shaped (n_spot, n_vol, n_time)
// (the C++ row-major [spot][vol][time] order); exclusion counts gain a trailing axis of
// length len(ScenarioExclusion).
//
// ServedSession (analytics/served_session.hpp) is bound over a PricedSurface only: the
// VolaSession overload needs a VolaSession binding, which does not exist. The session
// BORROWS the surface (keep_alive ties the surface to the session object) and co-owns the
// book (ServeBook's holder is std::shared_ptr, serve.cpp).
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/api/analytics/served_analytics.hpp"
#include "atx/vol/api/analytics/served_session.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
#include "atx/vol/api/fitting/serve_book.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"
#include "common.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

py::bytes digest_bytes(const ServeDigest &d) {
  return py::bytes(reinterpret_cast<const char *>(d.data()), d.size()); // SAFETY: byte read
}

py::list hedge_reasons(const std::array<HedgeGreekReason, kHedgeGreekCount> &r) {
  py::list out;
  for (const HedgeGreekReason g : r) out.append(py::cast(g));
  return out;
}

// ── scenario grids ──────────────────────────────────────────────────────────────────

std::vector<py::ssize_t> grid_shape(std::size_t ns, std::size_t nv, std::size_t nt) {
  return {static_cast<py::ssize_t>(ns), static_cast<py::ssize_t>(nv),
          static_cast<py::ssize_t>(nt)};
}

template <class F>
py::array_t<double> cell_grid(const std::vector<ScenarioCell> &cells, std::size_t ns,
                              std::size_t nv, std::size_t nt, F field) {
  py::array_t<double> out(grid_shape(ns, nv, nt));
  double *p = out.mutable_data();
  for (std::size_t i = 0; i < cells.size(); ++i) p[i] = static_cast<double>(field(cells[i]));
  return out;
}

template <class T, std::size_t N, class F>
py::array_t<T> cell_counts(const std::vector<ScenarioCell> &cells, std::size_t ns,
                           std::size_t nv, std::size_t nt, F field) {
  std::vector<py::ssize_t> shape = grid_shape(ns, nv, nt);
  shape.push_back(static_cast<py::ssize_t>(N));
  py::array_t<T> out(shape);
  T *p = out.mutable_data();
  for (std::size_t i = 0; i < cells.size(); ++i) {
    const auto &arr = field(cells[i]);
    for (std::size_t j = 0; j < N; ++j) p[i * N + j] = arr[j];
  }
  return out;
}

py::dict cells_dict(const std::vector<ScenarioCell> &c, std::size_t ns, std::size_t nv,
                    std::size_t nt) {
  if (c.size() != ns * nv * nt) throw std::runtime_error("scenario cell count mismatch");
  py::dict d;
  d["spot_rel"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.spot_rel; });
  d["vol_abs"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.vol_abs; });
  d["time_years"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.time_years; });
  d["taylor_pnl"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.taylor_pnl; });
  d["taylor_delta"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.taylor_delta; });
  d["taylor_gamma"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.taylor_gamma; });
  d["taylor_vega"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.taylor_vega; });
  d["taylor_theta"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.taylor_theta; });
  d["taylor_remainder"] =
      cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.taylor_remainder; });
  d["exact_pnl"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.exact_pnl; });
  d["exact_certificate_usd"] =
      cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.exact_certificate_usd; });
  d["estimate_taylor_pnl"] =
      cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.estimate_taylor_pnl; });
  d["estimate_exact_pnl"] =
      cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.estimate_exact_pnl; });
  d["matched_taylor_pnl"] =
      cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.matched_taylor_pnl; });
  d["matched_exact_pnl"] =
      cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.matched_exact_pnl; });
  d["matched_remainder"] =
      cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.matched_remainder; });
  d["taylor_legs"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.taylor_legs; });
  d["exact_legs"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.exact_legs; });
  d["matched_legs"] = cell_grid(c, ns, nv, nt, [](const ScenarioCell &x) { return x.matched_legs; });
  d["exact_routes"] = cell_counts<std::uint32_t, 5>(
      c, ns, nv, nt, [](const ScenarioCell &x) -> const auto & { return x.exact_routes; });
  d["taylor_excluded"] = cell_counts<std::uint32_t, kScenarioExclusionCount>(
      c, ns, nv, nt, [](const ScenarioCell &x) -> const auto & { return x.taylor_excluded; });
  d["taylor_excluded_usd"] = cell_counts<double, kScenarioExclusionCount>(
      c, ns, nv, nt, [](const ScenarioCell &x) -> const auto & { return x.taylor_excluded_usd; });
  d["exact_excluded"] = cell_counts<std::uint32_t, kScenarioExclusionCount>(
      c, ns, nv, nt, [](const ScenarioCell &x) -> const auto & { return x.exact_excluded; });
  d["exact_excluded_usd"] = cell_counts<double, kScenarioExclusionCount>(
      c, ns, nv, nt, [](const ScenarioCell &x) -> const auto & { return x.exact_excluded_usd; });
  return d;
}

py::dict workload_dict(const ServedWorkload &w) {
  py::dict d;
  d["contracts"] = w.contracts;
  d["unique_contracts"] = w.unique_contracts;
  d["greek_mask"] = w.greek_mask.bits;
  d["n_spot"] = w.n_spot;
  d["n_vol"] = w.n_vol;
  d["n_time"] = w.n_time;
  d["n_cells"] = w.n_cells;
  d["expiries"] = w.expiries;
  d["events_in_horizon"] = w.events_in_horizon;
  return d;
}

void bind_enums(py::module_ &m) {
  namespace pyx = atxvol::python;
  py::enum_<AnalyticsModel>(m, "AnalyticsModel")
      .value("NONE", AnalyticsModel::None)
      .value("QUOTING_RECORD", AnalyticsModel::QuotingRecord)
      .value("COLD_KERNEL", AnalyticsModel::ColdKernel)
      .value("EVENT_SETTLEMENT", AnalyticsModel::EventSettlement)
      .value("DISCRETE_DIVIDEND", AnalyticsModel::DiscreteDividend)
      .value("SESSION_CURVE", AnalyticsModel::SessionCurve)
      .value("COMMON_MODEL_EUROPEAN", AnalyticsModel::CommonModelEuropean);
  pyx::register_enum_names<AnalyticsModel>("AnalyticsModel");
  py::enum_<HedgeConvention>(m, "HedgeConvention")
      .value("CONTRACT_SIGMA", HedgeConvention::ContractSigma)
      .value("STICKY_STRIKE", HedgeConvention::StickyStrike)
      .value("STICKY_DELTA_MONEYNESS", HedgeConvention::StickyDeltaMoneyness)
      .value("REFITTED_SURFACE", HedgeConvention::RefittedSurface);
  pyx::register_enum_names<HedgeConvention>("HedgeConvention");
  py::enum_<AnalyticsInputClass>(m, "AnalyticsInputClass")
      .value("CERTIFIED", AnalyticsInputClass::Certified)
      .value("ESTIMATE", AnalyticsInputClass::Estimate)
      .value("PRICE_ONLY", AnalyticsInputClass::PriceOnly)
      .value("REFUSED", AnalyticsInputClass::Refused);
  pyx::register_enum_names<AnalyticsInputClass>("AnalyticsInputClass");
  py::enum_<AnalyticsStatus>(m, "AnalyticsStatus")
      .value("OK", AnalyticsStatus::Ok)
      .value("ESTIMATE", AnalyticsStatus::Estimate)
      .value("PRICE_ONLY", AnalyticsStatus::PriceOnly)
      .value("REFUSED", AnalyticsStatus::Refused)
      .value("VERSION_MOVED", AnalyticsStatus::VersionMoved)
      .value("INVALID_ARGUMENT", AnalyticsStatus::InvalidArgument);
  pyx::register_enum_names<AnalyticsStatus>("AnalyticsStatus");
  py::enum_<HedgeGreekReason>(m, "HedgeGreekReason")
      .value("NOT_REQUESTED", HedgeGreekReason::NotRequested)
      .value("SERVED", HedgeGreekReason::Served)
      .value("EXERCISED", HedgeGreekReason::Exercised)
      .value("REFUSED", HedgeGreekReason::Refused)
      .value("NO_CARRY_BUMPS", HedgeGreekReason::NoCarryBumps)
      .value("CARRY_BUMP_REFUSED", HedgeGreekReason::CarryBumpRefused)
      .value("NO_Q_AXIS", HedgeGreekReason::NoQAxis)
      .value("NEAR_NODE_DEGENERATE", HedgeGreekReason::NearNodeDegenerate)
      .value("CONVENTION_UNSUPPORTED", HedgeGreekReason::ConventionUnsupported)
      .value("NEAR_EXERCISE_EDGE", HedgeGreekReason::NearExerciseEdge)
      .value("BUNDLE_UNAVAILABLE", HedgeGreekReason::BundleUnavailable)
      .value("TIER_HAS_NO_GREEK", HedgeGreekReason::TierHasNoGreek)
      .value("NO_MAP", HedgeGreekReason::NoMap)
      .value("SMILE_SLOPE_UNAVAILABLE", HedgeGreekReason::SmileSlopeUnavailable)
      .value("NEEDS_VANNA_VOLGA", HedgeGreekReason::NeedsVannaVolga)
      .value("REQUIRES_REFIT", HedgeGreekReason::RequiresRefit);
  pyx::register_enum_names<HedgeGreekReason>("HedgeGreekReason");
  py::enum_<SmileSlopeClass>(m, "SmileSlopeClass")
      .value("NONE", SmileSlopeClass::None)
      .value("ZERO_BY_CONVENTION", SmileSlopeClass::ZeroByConvention)
      .value("FITTED_CURVE_CENTRAL_DIFFERENCE", SmileSlopeClass::FittedCurveCentralDifference);
  py::enum_<ScenarioMethod>(m, "ScenarioMethod")
      .value("TAYLOR", ScenarioMethod::Taylor)
      .value("EXACT", ScenarioMethod::Exact)
      .value("BOTH", ScenarioMethod::Both);
  py::enum_<ScenarioExactRoute>(m, "ScenarioExactRoute")
      .value("NONE", ScenarioExactRoute::None)
      .value("RECORD_TIER", ScenarioExactRoute::RecordTier)
      .value("COLD_KERNEL", ScenarioExactRoute::ColdKernel)
      .value("EVENT_SETTLEMENT", ScenarioExactRoute::EventSettlement)
      .value("DISCRETE_DIVIDEND", ScenarioExactRoute::DiscreteDividend);
  pyx::register_enum_names<ScenarioExactRoute>("ScenarioExactRoute");
  py::enum_<ScenarioExclusion>(m, "ScenarioExclusion")
      .value("NONE", ScenarioExclusion::None)
      .value("REFUSED", ScenarioExclusion::Refused)
      .value("PRICE_ONLY", ScenarioExclusion::PriceOnly)
      .value("ESTIMATE", ScenarioExclusion::Estimate)
      .value("MISSING_GREEK", ScenarioExclusion::MissingGreek)
      .value("OUTSIDE_TAYLOR_BOX", ScenarioExclusion::OutsideTaylorBox)
      .value("REMAINDER_UNBOUNDED", ScenarioExclusion::RemainderUnbounded)
      .value("EVENT_OPERATION", ScenarioExclusion::EventOperation)
      .value("EVENT_REQUIRES_REFIT", ScenarioExclusion::EventRequiresRefit)
      .value("EXACT_REFUSED", ScenarioExclusion::ExactRefused)
      .value("OFF_NODE", ScenarioExclusion::OffNode);
  pyx::register_enum_names<ScenarioExclusion>("ScenarioExclusion");
  m.attr("SCENARIO_SIGMA_FLOOR") = kScenarioSigmaFloor;
  m.attr("SMILE_SLOPE_STEP") = kSmileSlopeStep;
}

void bind_declaration(py::module_ &m) {
  py::class_<AnalyticsClock>(m, "AnalyticsClock")
      .def_readonly("now_ns", &AnalyticsClock::now_ns)
      .def_readonly("as_of_ns", &AnalyticsClock::as_of_ns)
      .def_readonly("expiry_ns", &AnalyticsClock::expiry_ns)
      .def_readonly("next_event_ns", &AnalyticsClock::next_event_ns)
      .def_readonly("tau", &AnalyticsClock::tau);
  py::class_<AnalyticsDeclaration>(m, "AnalyticsDeclaration",
                                   "What produced an analytics number (A1 declaration).")
      .def_readonly("book_version", &AnalyticsDeclaration::book_version)
      .def_readonly("expiry_id", &AnalyticsDeclaration::expiry_id)
      .def_readonly("entry_tag", &AnalyticsDeclaration::entry_tag)
      .def_property_readonly("record_fit_sha256",
                             [](const AnalyticsDeclaration &d) { return digest_bytes(d.record_fit_sha256); })
      .def_property_readonly("record_policy_sha256",
                             [](const AnalyticsDeclaration &d) {
                               return digest_bytes(d.record_policy_sha256);
                             })
      .def_property_readonly("record_input_sha256",
                             [](const AnalyticsDeclaration &d) {
                               return digest_bytes(d.record_input_sha256);
                             })
      .def_readonly("model", &AnalyticsDeclaration::model)
      .def_readonly("greek_model", &AnalyticsDeclaration::greek_model)
      .def_readonly("convention", &AnalyticsDeclaration::convention)
      .def_readonly("clock", &AnalyticsDeclaration::clock)
      .def_readonly("price_map_delta", &AnalyticsDeclaration::price_map_delta);

  m.def("classify_served_answer", &classify_served_answer, py::arg("answer"),
        "The AnalyticsInputClass of a ServeAnswer (Certified / Estimate / PriceOnly / Refused).");
}

void bind_hedge(py::module_ &m) {
  py::class_<HedgeGreeksRequest>(m, "HedgeGreeksRequest")
      .def(py::init([](std::uint64_t expiry_id, double strike, Side side, double spot,
                       std::int64_t now_ns, HedgeConvention convention, ServeTier tier,
                       ServeTier tier_max) {
             return HedgeGreeksRequest{expiry_id, strike, side, spot, now_ns, convention, tier,
                                       tier_max};
           }),
           py::arg("expiry_id"), py::arg("strike"), py::arg("side"), py::arg("spot"),
           py::arg("now_ns") = 0, py::arg("convention") = HedgeConvention::StickyStrike,
           py::arg("tier") = ServeTier::Ladder, py::arg("tier_max") = ServeTier::Family)
      .def_readwrite("expiry_id", &HedgeGreeksRequest::expiry_id)
      .def_readwrite("strike", &HedgeGreeksRequest::strike)
      .def_readwrite("side", &HedgeGreeksRequest::side)
      .def_readwrite("spot", &HedgeGreeksRequest::spot)
      .def_readwrite("now_ns", &HedgeGreeksRequest::now_ns)
      .def_readwrite("convention", &HedgeGreeksRequest::convention)
      .def_readwrite("tier", &HedgeGreeksRequest::tier)
      .def_readwrite("tier_max", &HedgeGreeksRequest::tier_max);

  py::class_<HedgeGreeks>(m, "HedgeGreeks")
      .def_readonly("price", &HedgeGreeks::price)
      .def_readonly("delta", &HedgeGreeks::delta)
      .def_readonly("gamma", &HedgeGreeks::gamma)
      .def_readonly("vega", &HedgeGreeks::vega)
      .def_readonly("theta", &HedgeGreeks::theta)
      .def_property_readonly("reason", [](const HedgeGreeks &h) { return hedge_reasons(h.reason); },
                             "delta, gamma, vega, theta")
      .def_readonly("sigma", &HedgeGreeks::sigma)
      .def_readonly("dsigma_dspot", &HedgeGreeks::dsigma_dspot)
      .def_readonly("curve_slope", &HedgeGreeks::curve_slope)
      .def_readonly("slope_class", &HedgeGreeks::slope_class)
      .def_readonly("certificate_usd", &HedgeGreeks::certificate_usd)
      .def_readonly("certificate_class", &HedgeGreeks::certificate_class)
      .def_readonly("sampled", &HedgeGreeks::sampled)
      .def_readonly("price_only_reason", &HedgeGreeks::price_only_reason)
      .def_readonly("iv_lower", &HedgeGreeks::iv_lower)
      .def_readonly("iv_upper", &HedgeGreeks::iv_upper)
      .def_readonly("edge_sd", &HedgeGreeks::edge_sd)
      .def_readonly("near_edge", &HedgeGreeks::near_edge)
      .def_readonly("serve_status", &HedgeGreeks::serve_status)
      .def_readonly("route", &HedgeGreeks::route)
      .def_readonly("tier", &HedgeGreeks::tier)
      .def_readonly("input_class", &HedgeGreeks::input_class)
      .def_readonly("status", &HedgeGreeks::status)
      .def_readonly("declaration", &HedgeGreeks::declaration);

  m.def("hedge_greeks", &hedge_greeks, py::arg("reader"), py::arg("request"),
        "One served answer turned into the convention's hedge Greeks (typed status).");
}

void bind_scenarios(py::module_ &m) {
  py::class_<ServedPosition>(m, "ServedPosition")
      .def(py::init([](std::uint64_t leg_id, std::uint64_t expiry_id, double strike, Side side,
                       double quantity, double multiplier) {
             return ServedPosition{leg_id, expiry_id, strike, side, quantity, multiplier};
           }),
           py::arg("leg_id"), py::arg("expiry_id"), py::arg("strike"), py::arg("side"),
           py::arg("quantity"), py::arg("multiplier") = 100.0)
      .def_readwrite("leg_id", &ServedPosition::leg_id)
      .def_readwrite("expiry_id", &ServedPosition::expiry_id)
      .def_readwrite("strike", &ServedPosition::strike)
      .def_readwrite("side", &ServedPosition::side)
      .def_readwrite("quantity", &ServedPosition::quantity)
      .def_readwrite("multiplier", &ServedPosition::multiplier);

  py::class_<TaylorBox>(m, "TaylorBox")
      .def(py::init([](double spot_rel, double vol_abs, double time_years) {
             return TaylorBox{spot_rel, vol_abs, time_years};
           }),
           py::arg("spot_rel") = 0.03, py::arg("vol_abs") = 0.03,
           py::arg("time_years") = 7.0 / 365.0)
      .def_readwrite("spot_rel", &TaylorBox::spot_rel)
      .def_readwrite("vol_abs", &TaylorBox::vol_abs)
      .def_readwrite("time_years", &TaylorBox::time_years);
  py::class_<TaylorOrder>(m, "TaylorOrder")
      .def_readonly("spot", &TaylorOrder::spot)
      .def_readonly("vol", &TaylorOrder::vol)
      .def_readonly("time", &TaylorOrder::time)
      .def_readonly("cross_terms", &TaylorOrder::cross_terms);

  py::class_<ScenarioSpec>(m, "ScenarioSpec")
      .def(py::init<>())
      .def_readwrite("spot_rel", &ScenarioSpec::spot_rel)
      .def_readwrite("vol_abs", &ScenarioSpec::vol_abs)
      .def_readwrite("time_years", &ScenarioSpec::time_years)
      .def_readwrite("spot", &ScenarioSpec::spot)
      .def_readwrite("now_ns", &ScenarioSpec::now_ns)
      .def_readwrite("method", &ScenarioSpec::method)
      .def_readwrite("box", &ScenarioSpec::box)
      .def_readwrite("cold_opts", &ScenarioSpec::cold_opts)
      .def_readwrite("tier", &ScenarioSpec::tier)
      .def_readwrite("tier_max", &ScenarioSpec::tier_max)
      .def_readwrite("workers", &ScenarioSpec::workers)
      .def_readwrite("keep_legs", &ScenarioSpec::keep_legs)
      .def_readwrite("convention", &ScenarioSpec::convention);

  py::class_<ServedLegProvenance>(m, "ServedLegProvenance")
      .def_readonly("leg_id", &ServedLegProvenance::leg_id)
      .def_readonly("unique_index", &ServedLegProvenance::unique_index)
      .def_readonly("base_price", &ServedLegProvenance::base_price)
      .def_readonly("cold_base_price", &ServedLegProvenance::cold_base_price)
      .def_readonly("delta", &ServedLegProvenance::delta)
      .def_readonly("gamma", &ServedLegProvenance::gamma)
      .def_readonly("vega", &ServedLegProvenance::vega)
      .def_readonly("theta", &ServedLegProvenance::theta)
      .def_property_readonly("reason",
                             [](const ServedLegProvenance &l) { return hedge_reasons(l.reason); })
      .def_readonly("sigma", &ServedLegProvenance::sigma)
      .def_readonly("edge_sd", &ServedLegProvenance::edge_sd)
      .def_readonly("certificate_usd", &ServedLegProvenance::certificate_usd)
      .def_readonly("certificate_class", &ServedLegProvenance::certificate_class)
      .def_readonly("sampled", &ServedLegProvenance::sampled)
      .def_readonly("price_only_reason", &ServedLegProvenance::price_only_reason)
      .def_readonly("serve_status", &ServedLegProvenance::serve_status)
      .def_readonly("route", &ServedLegProvenance::route)
      .def_readonly("tier", &ServedLegProvenance::tier)
      .def_readonly("input_class", &ServedLegProvenance::input_class)
      .def_readonly("declaration", &ServedLegProvenance::declaration);

  py::class_<ScenarioResult>(m, "ScenarioResult")
      .def_readonly("status", &ScenarioResult::status)
      .def_readonly("book_version", &ScenarioResult::book_version)
      .def_property_readonly("workload",
                             [](const ScenarioResult &r) { return workload_dict(r.workload); })
      .def_readonly("order", &ScenarioResult::order)
      .def_readonly("box", &ScenarioResult::box)
      .def_readonly("convention", &ScenarioResult::convention)
      .def_readonly("n_spot", &ScenarioResult::n_spot)
      .def_readonly("n_vol", &ScenarioResult::n_vol)
      .def_readonly("n_time", &ScenarioResult::n_time)
      .def_property_readonly("cells",
                             [](const ScenarioResult &r) {
                               return cells_dict(r.cell, r.n_spot, r.n_vol, r.n_time);
                             },
                             "dict of numpy arrays shaped (n_spot, n_vol, n_time[, k]).")
      .def_readonly("legs", &ScenarioResult::legs)
      .def_readonly("attempts", &ScenarioResult::attempts)
      .def_readonly("base_serves", &ScenarioResult::base_serves)
      .def_readonly("exact_record_serves", &ScenarioResult::exact_record_serves)
      .def_readonly("cold_solves", &ScenarioResult::cold_solves);

  m.def(
      "served_scenarios",
      [](const ServeBook &book, const std::vector<ServedPosition> &portfolio,
         const ScenarioSpec &spec) {
        Result<ScenarioResult> result = [&] {
          py::gil_scoped_release release;
          return served_scenarios(book, portfolio, spec);
        }();
        return atxvol::python::unwrap(std::move(result));
      },
      py::arg("book"), py::arg("portfolio"), py::arg("spec"),
      "The scenario matrix of a portfolio on one book version; AtxError(INVALID_ARGUMENT) "
      "for an empty axis / non-finite shock / unsupported convention.");

  py::class_<ServedMove>(m, "ServedMove")
      .def(py::init([](double spot_rel, double vol_abs, double time_years) {
             return ServedMove{spot_rel, vol_abs, time_years};
           }),
           py::arg("spot_rel") = 0.0, py::arg("vol_abs") = 0.0, py::arg("time_years") = 0.0)
      .def_readwrite("spot_rel", &ServedMove::spot_rel)
      .def_readwrite("vol_abs", &ServedMove::vol_abs)
      .def_readwrite("time_years", &ServedMove::time_years);
  py::class_<ServedLegAttribution>(m, "ServedLegAttribution")
      .def_readonly("leg_id", &ServedLegAttribution::leg_id)
      .def_readonly("delta", &ServedLegAttribution::delta)
      .def_readonly("gamma", &ServedLegAttribution::gamma)
      .def_readonly("vega", &ServedLegAttribution::vega)
      .def_readonly("theta", &ServedLegAttribution::theta)
      .def_readonly("explained", &ServedLegAttribution::explained)
      .def_readonly("actual", &ServedLegAttribution::actual)
      .def_readonly("unexplained", &ServedLegAttribution::unexplained)
      .def_readonly("route", &ServedLegAttribution::route)
      .def_readonly("taylor_exclusion", &ServedLegAttribution::taylor_exclusion)
      .def_readonly("exact_exclusion", &ServedLegAttribution::exact_exclusion);
  py::class_<ServedAttribution>(m, "ServedAttribution")
      .def_readonly("status", &ServedAttribution::status)
      .def_readonly("move", &ServedAttribution::move)
      .def_readonly("legs", &ServedAttribution::legs)
      .def_readonly("delta", &ServedAttribution::delta)
      .def_readonly("gamma", &ServedAttribution::gamma)
      .def_readonly("vega", &ServedAttribution::vega)
      .def_readonly("theta", &ServedAttribution::theta)
      .def_readonly("explained", &ServedAttribution::explained)
      .def_readonly("actual", &ServedAttribution::actual)
      .def_readonly("unexplained", &ServedAttribution::unexplained)
      .def_readonly("attributed_legs", &ServedAttribution::attributed_legs)
      .def_readonly("scenario", &ServedAttribution::scenario);
  m.def(
      "served_attribution",
      [](const ServeBook &book, const std::vector<ServedPosition> &portfolio,
         const ServedMove &move, const ScenarioSpec &spec) {
        Result<ServedAttribution> result = [&] {
          py::gil_scoped_release release;
          return served_attribution(book, portfolio, move, spec);
        }();
        return atxvol::python::unwrap(std::move(result));
      },
      py::arg("book"), py::arg("portfolio"), py::arg("move"), py::arg("spec"));
}

void bind_cash(py::module_ &m) {
  py::class_<CashDividendContract>(m, "CashDividendContract")
      .def(py::init([](std::uint64_t leg_id, double spot, double strike, double tau, double sigma,
                       double rate, double yield, Side side, ExerciseStyle exercise,
                       const std::vector<std::pair<double, double>> &dividends, double quantity,
                       double multiplier) {
             CashDividendContract c;
             c.leg_id = leg_id;
             c.spot = spot;
             c.strike = strike;
             c.tau = tau;
             c.sigma = sigma;
             c.rate = rate;
             c.yield = yield;
             c.side = side;
             c.exercise = exercise;
             for (const auto &[t, a] : dividends) c.dividends.push_back(CashDividend{t, a});
             c.quantity = quantity;
             c.multiplier = multiplier;
             return c;
           }),
           py::arg("leg_id"), py::arg("spot"), py::arg("strike"), py::arg("tau"),
           py::arg("sigma"), py::arg("rate"), py::arg("yield_") = 0.0, py::arg("side") = Side::Call,
           py::arg("exercise") = ExerciseStyle::American,
           py::arg("dividends") = std::vector<std::pair<double, double>>{},
           py::arg("quantity") = 1.0, py::arg("multiplier") = 100.0);

  py::class_<CashDividendLegCell>(m, "CashDividendLegCell")
      .def_readonly("taylor", &CashDividendLegCell::taylor)
      .def_readonly("remainder", &CashDividendLegCell::remainder)
      .def_readonly("exact", &CashDividendLegCell::exact)
      .def_readonly("events_crossed", &CashDividendLegCell::events_crossed)
      .def_readonly("cash_crossed", &CashDividendLegCell::cash_crossed)
      .def_readonly("exact_route", &CashDividendLegCell::exact_route)
      .def_readonly("taylor_exclusion", &CashDividendLegCell::taylor_exclusion)
      .def_readonly("exact_exclusion", &CashDividendLegCell::exact_exclusion);
  py::class_<CashDividendLeg>(m, "CashDividendLeg")
      .def_readonly("leg_id", &CashDividendLeg::leg_id)
      .def_property_readonly("base_price", [](const CashDividendLeg &l) { return l.base.price; })
      .def_property_readonly("base_unsupported",
                             [](const CashDividendLeg &l) { return l.base.unsupported; })
      .def_readonly("declaration", &CashDividendLeg::declaration)
      .def_readonly("cells", &CashDividendLeg::cells);
  py::class_<CashDividendScenarioResult>(m, "CashDividendScenarioResult")
      .def_readonly("n_spot", &CashDividendScenarioResult::n_spot)
      .def_readonly("n_vol", &CashDividendScenarioResult::n_vol)
      .def_readonly("n_time", &CashDividendScenarioResult::n_time)
      .def_readonly("order", &CashDividendScenarioResult::order)
      .def_readonly("box", &CashDividendScenarioResult::box)
      .def_readonly("legs", &CashDividendScenarioResult::legs)
      .def_property_readonly("cells",
                             [](const CashDividendScenarioResult &r) {
                               return cells_dict(r.cell, r.n_spot, r.n_vol, r.n_time);
                             })
      .def_readonly("lattice_solves", &CashDividendScenarioResult::lattice_solves);

  m.def(
      "cash_dividend_scenarios",
      [](const std::vector<CashDividendContract> &contracts, const std::vector<double> &spot_rel,
         const std::vector<double> &vol_abs, const std::vector<double> &time_years,
         const TaylorBox &box) {
        CashDividendScenarioSpec spec;
        spec.spot_rel = spot_rel;
        spec.vol_abs = vol_abs;
        spec.time_years = time_years;
        spec.box = box;
        Result<CashDividendScenarioResult> result = [&] {
          py::gil_scoped_release release;
          return cash_dividend_scenarios(contracts, spec);
        }();
        return atxvol::python::unwrap(std::move(result));
      },
      py::arg("contracts"), py::arg("spot_rel") = std::vector<double>{0.0},
      py::arg("vol_abs") = std::vector<double>{0.0},
      py::arg("time_years") = std::vector<double>{0.0}, py::arg("box") = TaylorBox{},
      "Dated-cash scenario matrix on D1's production lattice (default route options).");
}

// ── ServedSession (PricedSurface overload) ──────────────────────────────────────────

template <class T> py::array_t<T> column_of(std::size_t n) {
  return py::array_t<T>(static_cast<py::ssize_t>(n));
}

py::dict session_chain(const ServedSession &session, ServeBookReader &reader, double spot,
                       std::int64_t now_ns, const atxvol::python::F64Column &strike,
                       const atxvol::python::F64Column &expiry, const py::object &side) {
  const std::size_t n = static_cast<std::size_t>(strike.size());
  const auto k = atxvol::python::column(strike, "strike", n, "strike");
  const auto t = atxvol::python::column(expiry, "expiry", n, "strike");
  const std::vector<Side> sides = atxvol::python::side_column(side, n, "strike");
  auto price = column_of<double>(n), delta = column_of<double>(n), gamma = column_of<double>(n),
       vega = column_of<double>(n), theta = column_of<double>(n);
  std::vector<TickStatus> ticks(n);
  std::vector<SessionChainStatement> statements(n);
  std::vector<std::uint64_t> scratch(n);
  const ChainTickOutputs out{{price.mutable_data(), n}, {delta.mutable_data(), n},
                             {gamma.mutable_data(), n}, {vega.mutable_data(), n},
                             {theta.mutable_data(), n}, ticks};
  const SessionChainView chain{k, t, sides};
  Status status;
  {
    py::gil_scoped_release release;
    status = session.serve_chain(reader, spot, now_ns, chain, out, statements, scratch);
  }
  atxvol::python::unwrap(status);
  auto tick = column_of<std::uint8_t>(n), source = column_of<std::uint8_t>(n),
       input = column_of<std::uint8_t>(n), st = column_of<std::uint8_t>(n),
       route = column_of<std::uint8_t>(n), tier = column_of<std::uint8_t>(n),
       cls = column_of<std::uint8_t>(n);
  auto cert = column_of<double>(n);
  auto sampled = column_of<bool>(n);
  for (std::size_t i = 0; i < n; ++i) {
    const SessionChainStatement &s = statements[i];
    tick.mutable_data()[i] = static_cast<std::uint8_t>(ticks[i]);
    source.mutable_data()[i] = static_cast<std::uint8_t>(s.source);
    input.mutable_data()[i] = static_cast<std::uint8_t>(s.input_class);
    st.mutable_data()[i] = static_cast<std::uint8_t>(s.book.status);
    route.mutable_data()[i] = static_cast<std::uint8_t>(s.book.route);
    tier.mutable_data()[i] = static_cast<std::uint8_t>(s.book.tier);
    cls.mutable_data()[i] = static_cast<std::uint8_t>(s.book.certificate_class);
    cert.mutable_data()[i] = s.book.certificate_usd;
    sampled.mutable_data()[i] = s.book.sampled;
  }
  py::dict d;
  d["price"] = std::move(price);
  d["delta"] = std::move(delta);
  d["gamma"] = std::move(gamma);
  d["vega"] = std::move(vega);
  d["theta"] = std::move(theta);
  d["tick_status"] = std::move(tick);
  d["source"] = std::move(source);
  d["input_class"] = std::move(input);
  d["status"] = std::move(st);
  d["route"] = std::move(route);
  d["tier"] = std::move(tier);
  d["certificate_class"] = std::move(cls);
  d["certificate_usd"] = std::move(cert);
  d["sampled"] = std::move(sampled);
  return d;
}

void bind_session(py::module_ &m) {
  py::enum_<SessionServeSource>(m, "SessionServeSource")
      .value("BOOK", SessionServeSource::Book)
      .value("SESSION_COLD", SessionServeSource::SessionCold)
      .value("REFUSED", SessionServeSource::Refused);
  atxvol::python::register_enum_names<SessionServeSource>("SessionServeSource");

  py::class_<SessionBookExpiry>(m, "SessionBookExpiry")
      .def(py::init([](double T, std::uint64_t expiry_id) { return SessionBookExpiry{T, expiry_id}; }),
           py::arg("T"), py::arg("expiry_id"))
      .def_readwrite("T", &SessionBookExpiry::T)
      .def_readwrite("expiry_id", &SessionBookExpiry::expiry_id);

  py::class_<SessionServedAnswer>(m, "SessionServedAnswer")
      .def_readonly("price", &SessionServedAnswer::price)
      .def_readonly("delta", &SessionServedAnswer::delta)
      .def_readonly("gamma", &SessionServedAnswer::gamma)
      .def_readonly("vega", &SessionServedAnswer::vega)
      .def_readonly("theta", &SessionServedAnswer::theta)
      .def_readonly("source", &SessionServedAnswer::source)
      .def_readonly("book", &SessionServedAnswer::book)
      .def_readonly("input_class", &SessionServedAnswer::input_class)
      .def_readonly("status", &SessionServedAnswer::status)
      .def_readonly("declaration", &SessionServedAnswer::declaration);

  py::class_<ServedSession>(m, "ServedSession",
                            "A PricedSurface served through a ServeBook at the bound expiries.")
      .def_static(
          "bind",
          [](const PricedSurface &surface, std::shared_ptr<ServeBook> book,
             const std::vector<SessionBookExpiry> &expiries) {
            if (!book) throw py::value_error("book must not be None");
            return atxvol::python::unwrap(ServedSession::bind(
                surface, std::shared_ptr<const ServeBook>(std::move(book)), expiries));
          },
          // The session borrows the surface: keep it alive as long as the session.
          py::keep_alive<0, 1>(), py::arg("surface"), py::arg("book"), py::arg("expiries"),
          "Bind (T -> book expiry id) pairs; AtxError(INVALID_ARGUMENT) for a bad T or a "
          "duplicate binding.")
      .def("reader", &ServedSession::reader, "A reader for the calling thread.")
      .def(
          "serve",
          [](const ServedSession &s, ServeBookReader &reader, double K, double T, Side side,
             double spot, std::int64_t now_ns) {
            py::gil_scoped_release release;
            return s.serve(reader, K, T, side, spot, now_ns);
          },
          py::arg("reader"), py::arg("K"), py::arg("T"), py::arg("side"), py::arg("spot"),
          py::arg("now_ns"),
          "The book's record at a bound T, else the surface's cold route; typed status.")
      .def("serve_chain", &session_chain, py::arg("reader"), py::arg("spot"), py::arg("now_ns"),
           py::arg("strike"), py::arg("expiry"), py::arg("side"),
           "numpy columns (price, Greeks, tick_status, source, input_class and the book "
           "statement's status / route / tier / certificate_class / certificate_usd / sampled).")
      .def(
          "bound_expiry",
          [](const ServedSession &s, double T) -> py::object {
            const auto id = s.bound_expiry(T);
            return id ? py::object(py::int_(*id)) : py::object(py::none());
          },
          py::arg("T"))
      .def("bindings", [](const ServedSession &s) {
        return std::vector<SessionBookExpiry>(s.bindings().begin(), s.bindings().end());
      });
}

} // namespace

void bind_served_analytics(py::module_ &m) {
  bind_enums(m);
  bind_declaration(m);
  bind_hedge(m);
  bind_scenarios(m);
  bind_cash(m);
  bind_session(m);
}
