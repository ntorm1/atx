// Bindings for the declarative strategy DSL and the dispersion-strangle spec
// builder.
//
// `StrategySpec` is plain data, so every sub-struct is exposed with a default
// constructor plus read/write fields; a spec can therefore be assembled field
// by field from Python, or produced wholesale by
// `make_dispersion_strangle_spec`. `DeclarativeStrategy` is bound through its
// `IStrategy` base so it can be handed straight to `run_backtest`.

#include <cstdint>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/dispersion.hpp"
#include "atx/vol/dispersion_strangle.hpp"
#include "atx/vol/strategy.hpp"
#include "atx/vol/universe.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;

void bind_strategy(py::module_ &m) {
  // ── Missing-name policy (shared by the DSL and the dispersion builder) ──
  py::enum_<MissingNamePolicy>(m, "MissingNamePolicy")
      .value("ERROR", MissingNamePolicy::Error)
      .value("DROP_RENORMALIZE", MissingNamePolicy::DropRenormalize);

  py::class_<MissingNameSpec>(m, "MissingNameSpec")
      .def(py::init<>())
      .def(py::init([](MissingNamePolicy policy, std::size_t min_names) {
             return MissingNameSpec{policy, min_names};
           }),
           py::arg("policy"), py::arg("min_names"))
      .def_readwrite("policy", &MissingNameSpec::policy)
      .def_readwrite("min_names", &MissingNameSpec::min_names);

  // ── Leg description ──
  py::class_<TenorSpec>(m, "TenorSpec")
      .def(py::init<>())
      .def_readwrite("target_T", &TenorSpec::target_T)
      .def_readwrite("snap_to_listed", &TenorSpec::snap_to_listed)
      .def_readwrite("snap_to_sessions", &TenorSpec::snap_to_sessions);

  py::class_<StrikeSelector> strike(m, "StrikeSelector");
  py::enum_<StrikeSelector::Kind>(strike, "Kind")
      .value("ATM_FORWARD", StrikeSelector::Kind::AtmForward)
      .value("DELTA", StrikeSelector::Kind::Delta)
      .value("MONEYNESS", StrikeSelector::Kind::Moneyness)
      .value("ABS_STRIKE", StrikeSelector::Kind::AbsStrike);
  strike.def(py::init<>())
      .def_readwrite("kind", &StrikeSelector::kind)
      .def_readwrite("value", &StrikeSelector::value);

  py::class_<StructureSpec> structure(m, "StructureSpec");
  py::enum_<StructureSpec::Kind>(structure, "Kind")
      .value("SINGLE", StructureSpec::Kind::Single)
      .value("STRADDLE", StructureSpec::Kind::Straddle)
      .value("STRANGLE", StructureSpec::Kind::Strangle)
      .value("RISK_REVERSAL", StructureSpec::Kind::RiskReversal);
  structure.def(py::init<>())
      .def_readwrite("kind", &StructureSpec::kind)
      .def_readwrite("single_side", &StructureSpec::single_side)
      .def_readwrite("call_leg", &StructureSpec::call_leg)
      .def_readwrite("put_leg", &StructureSpec::put_leg);

  py::class_<SizeSpec> size(m, "SizeSpec");
  py::enum_<SizeSpec::Kind>(size, "Kind")
      .value("FIXED_CONTRACTS", SizeSpec::Kind::FixedContracts)
      .value("TARGET_VEGA", SizeSpec::Kind::TargetVega)
      .value("WEIGHT", SizeSpec::Kind::Weight)
      .value("TARGET_THETA", SizeSpec::Kind::TargetTheta)
      .value("TARGET_GAMMA", SizeSpec::Kind::TargetGamma);
  size.def(py::init<>())
      .def_readwrite("kind", &SizeSpec::kind)
      .def_readwrite("value", &SizeSpec::value)
      .def_readwrite("sign", &SizeSpec::sign);

  py::class_<LegSpec>(m, "LegSpec")
      .def(py::init<>())
      .def_readwrite("symbol", &LegSpec::symbol)
      .def_readwrite("uid", &LegSpec::uid)
      .def_readwrite("tenor", &LegSpec::tenor)
      .def_readwrite("structure", &LegSpec::structure)
      .def_readwrite("strike", &LegSpec::strike)
      .def_readwrite("size", &LegSpec::size)
      .def_readwrite("group", &LegSpec::group);

  // ── Cross-leg constraint, lifecycle, hedging ──
  py::class_<CrossLegConstraint> constraint(m, "CrossLegConstraint");
  py::enum_<CrossLegConstraint::Kind>(constraint, "Kind")
      .value("NONE", CrossLegConstraint::Kind::None)
      .value("FLAT_VEGA", CrossLegConstraint::Kind::FlatVega)
      .value("VEGA_NEUTRAL_BASKET", CrossLegConstraint::Kind::VegaNeutralBasket);
  constraint.def(py::init<>())
      .def_readwrite("kind", &CrossLegConstraint::kind)
      .def_readwrite("group_a", &CrossLegConstraint::group_a)
      .def_readwrite("group_b", &CrossLegConstraint::group_b);

  py::class_<LifecycleSpec> lifecycle(m, "LifecycleSpec");
  py::enum_<LifecycleSpec::Entry>(lifecycle, "Entry")
      .value("EVERY_STEP", LifecycleSpec::Entry::EveryStep)
      .value("EVERY_N_DAYS", LifecycleSpec::Entry::EveryNDays);
  py::enum_<LifecycleSpec::Holding>(lifecycle, "Holding")
      .value("HOLD_TO_EXPIRY", LifecycleSpec::Holding::HoldToExpiry)
      .value("ROLL_AT_HORIZON", LifecycleSpec::Holding::RollAtHorizon)
      .value("CLOSE_AT_HORIZON", LifecycleSpec::Holding::CloseAtHorizon);
  lifecycle.def(py::init<>())
      .def_readwrite("entry", &LifecycleSpec::entry)
      .def_readwrite("holding", &LifecycleSpec::holding)
      .def_readwrite("entry_every_n", &LifecycleSpec::entry_every_n)
      .def_readwrite("roll_at_T", &LifecycleSpec::roll_at_T);

  py::class_<HedgeSpec> hedge(m, "HedgeSpec");
  py::enum_<HedgeSpec::Kind>(hedge, "Kind")
      .value("NONE", HedgeSpec::Kind::None)
      .value("DELTA_TO_ZERO", HedgeSpec::Kind::DeltaToZero);
  py::enum_<HedgeSpec::Cadence>(hedge, "Cadence")
      .value("AT_ENTRY", HedgeSpec::Cadence::AtEntry)
      .value("DAILY", HedgeSpec::Cadence::Daily);
  hedge.def(py::init<>())
      .def(py::init([](HedgeSpec::Kind kind, HedgeSpec::Cadence cadence, double band) {
             return HedgeSpec{kind, cadence, band};
           }),
           py::arg("kind"), py::arg("cadence") = HedgeSpec::Cadence::Daily, py::arg("band") = 0.0)
      .def_readwrite("kind", &HedgeSpec::kind)
      .def_readwrite("cadence", &HedgeSpec::cadence)
      .def_readwrite("band", &HedgeSpec::band);

  py::class_<ResolutionOptions>(m, "ResolutionOptions")
      .def(py::init<>())
      .def_readwrite("fast_screen_cold_confirm", &ResolutionOptions::fast_screen_cold_confirm)
      .def_readwrite("cold_delta_tolerance", &ResolutionOptions::cold_delta_tolerance)
      .def_readwrite("max_log_strike_step", &ResolutionOptions::max_log_strike_step)
      .def_readwrite("max_refine_iterations", &ResolutionOptions::max_refine_iterations);

  py::class_<StrategySpec>(m, "StrategySpec")
      .def(py::init<>())
      .def_readwrite("name", &StrategySpec::name)
      .def_readwrite("legs", &StrategySpec::legs)
      .def_readwrite("constraint", &StrategySpec::constraint)
      .def_readwrite("lifecycle", &StrategySpec::lifecycle)
      .def_readwrite("hedge", &StrategySpec::hedge)
      .def_readwrite("missing", &StrategySpec::missing)
      .def_readwrite("resolution", &StrategySpec::resolution)
      // list[int]: the run clock's snapshot timestamps, ascending. Only legs
      // with tenor.snap_to_sessions read it; the caller fills it from its Clock.
      .def_readwrite("session_ts", &StrategySpec::session_ts);

  // ── Strategies ──
  py::class_<IStrategy>(m, "IStrategy");

  py::class_<DeclarativeStrategy, IStrategy>(m, "DeclarativeStrategy")
      .def(py::init<StrategySpec>(), py::arg("spec"))
      .def_property_readonly("spec", &DeclarativeStrategy::spec,
                             py::return_value_policy::reference_internal)
      .def("hedge_spec", &DeclarativeStrategy::hedge_spec);

  // ── The dispersion-strangle spec builder ──
  py::class_<DispersionStrangleConfig>(m, "DispersionStrangleConfig")
      .def(py::init<>())
      .def_readwrite("names", &DispersionStrangleConfig::names)
      .def_readwrite("index_symbol", &DispersionStrangleConfig::index_symbol)
      .def_readwrite("target_abs_delta", &DispersionStrangleConfig::target_abs_delta)
      .def_readwrite("tenor_days", &DispersionStrangleConfig::tenor_days)
      .def_readwrite("close_dte_days", &DispersionStrangleConfig::close_dte_days)
      .def_readwrite("entry_every_n_days", &DispersionStrangleConfig::entry_every_n_days)
      .def_readwrite("theta_per_name_daily", &DispersionStrangleConfig::theta_per_name_daily)
      .def_readwrite("index_base_vega", &DispersionStrangleConfig::index_base_vega)
      .def_readwrite("missing", &DispersionStrangleConfig::missing)
      .def_readwrite("hedge", &DispersionStrangleConfig::hedge)
      .def_readwrite("hold_to_expiry", &DispersionStrangleConfig::hold_to_expiry)
      .def_readwrite("snap_expiry_to_sessions",
                     &DispersionStrangleConfig::snap_expiry_to_sessions);

  m.def(
      "make_dispersion_strangle_spec",
      [](const DispersionStrangleConfig &cfg) {
        return atxvol::python::unwrap(make_dispersion_strangle_spec(cfg));
      },
      py::arg("config"),
      "Assemble the long-names / short-index vega-flat strangle StrategySpec.");

  m.def("canonical_symbol", &canonical_symbol, py::arg("symbol"),
        "Canonical (upper-cased, trimmed) symbol form used by the resolvers.");
}
