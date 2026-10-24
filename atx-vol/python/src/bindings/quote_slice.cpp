// Quote-slice identifiability and snapshot metadata (lane C0, 2026-10-22).
//
// The raw American bid/ask slice fit (fit_quote_slice), its typed query
// (query_quote_slice) and the one-spot carry rebase (rebase_quote_slice_spot), with
// the per-row and per-fit metadata every consumer must be able to read: the typed
// price-only status and its reason / IV interval, the convention of each published
// volatility, the American model, and the snapshot provenance (quote and underlying
// clocks, spot and carry versions, carry convention, stale-spot flag).
//
// QuoteSliceConfig is exposed through its identifiability and admission scalars only;
// the curve, channel and repair sub-configs keep their library defaults here.
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "common.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

using DividendEvents = std::vector<std::pair<double, double>>;

// Dividends cross the boundary as (tau, amount) pairs, the CashDividend field order.
std::vector<CashDividend> dividends_of(const DividendEvents &events) {
  std::vector<CashDividend> out;
  out.reserve(events.size());
  for (const auto &[tau, amount] : events) out.push_back(CashDividend{tau, amount});
  return out;
}

std::vector<double> node_strikes(const QuoteSliceFit &fit) {
  std::vector<double> out;
  if (!fit.prices) return out;
  for (const AmericanPriceNode &node : fit.prices->nodes()) out.push_back(node.strike);
  return out;
}

void bind_enums(py::module_ &m) {
  py::enum_<QuoteSliceDividendModel>(m, "QuoteSliceDividendModel")
      .value("CONTINUOUS_YIELD", QuoteSliceDividendModel::ContinuousYield)
      .value("AGGREGATE_PRESENT_VALUE", QuoteSliceDividendModel::AggregatePresentValue)
      .value("AGGREGATE_FUNDING_PRESENT_VALUE",
             QuoteSliceDividendModel::AggregateFundingPresentValue)
      .value("DATED_CASH", QuoteSliceDividendModel::DatedCash);
  py::enum_<QuoteSliceQuoteStatus>(m, "QuoteSliceQuoteStatus")
      .value("ADMITTED", QuoteSliceQuoteStatus::Admitted)
      .value("INVALID_CONTRACT", QuoteSliceQuoteStatus::InvalidContract)
      .value("INVALID_QUOTE", QuoteSliceQuoteStatus::InvalidQuote)
      .value("EXCLUDED_SIDE", QuoteSliceQuoteStatus::ExcludedSide)
      .value("UNIDENTIFIABLE_VOLATILITY", QuoteSliceQuoteStatus::UnidentifiableVolatility)
      .value("IMPLIED_VOLATILITY_FAILURE", QuoteSliceQuoteStatus::ImpliedVolatilityFailure)
      .value("INVALID_OBSERVATION", QuoteSliceQuoteStatus::InvalidObservation)
      .value("BIDLESS_QUOTE", QuoteSliceQuoteStatus::BidlessQuote)
      .value("BELOW_MINIMUM_BID", QuoteSliceQuoteStatus::BelowMinimumBid)
      .value("WIDE_SPREAD_TO_MID", QuoteSliceQuoteStatus::WideSpreadToMid)
      .value("WIDE_SPREAD_VOL", QuoteSliceQuoteStatus::WideSpreadVol)
      .value("LOW_VEGA_WEIGHT", QuoteSliceQuoteStatus::LowVegaWeight);
  py::enum_<QuoteSliceEvaluationStatus>(m, "QuoteSliceEvaluationStatus")
      .value("SUCCESS", QuoteSliceEvaluationStatus::Success)
      .value("INVALID_CONTRACT", QuoteSliceEvaluationStatus::InvalidContract)
      .value("NO_FIT", QuoteSliceEvaluationStatus::NoFit)
      .value("INVALID_VOLATILITY", QuoteSliceEvaluationStatus::InvalidVolatility)
      .value("PRICING_FAILURE", QuoteSliceEvaluationStatus::PricingFailure)
      .value("PRICE_ONLY", QuoteSliceEvaluationStatus::PriceOnly)
      .value("MARKET_DISTANCE_FAILURE", QuoteSliceEvaluationStatus::MarketDistanceFailure);
  py::enum_<QuoteSliceFitStatus>(m, "QuoteSliceFitStatus")
      .value("SUCCESS", QuoteSliceFitStatus::Success)
      .value("INSUFFICIENT_QUOTES", QuoteSliceFitStatus::InsufficientQuotes)
      .value("FIT_FAILURE", QuoteSliceFitStatus::FitFailure)
      .value("ARBITRAGE_FAILURE", QuoteSliceFitStatus::ArbitrageFailure);
  py::enum_<QuoteSlicePriceOnlyReason>(m, "QuoteSlicePriceOnlyReason")
      .value("NONE", QuoteSlicePriceOnlyReason::None)
      .value("FLOOR_VOL", QuoteSlicePriceOnlyReason::FloorVol)
      .value("FLAT_EXERCISE_REGION", QuoteSlicePriceOnlyReason::FlatExerciseRegion)
      .value("LOW_VEGA", QuoteSlicePriceOnlyReason::LowVega)
      .value("UNIDENTIFIED_INTERVAL", QuoteSlicePriceOnlyReason::UnidentifiedInterval);
  py::enum_<QuoteSliceVolConvention>(m, "QuoteSliceVolConvention")
      .value("NONE", QuoteSliceVolConvention::None)
      .value("AMERICAN_QUOTING", QuoteSliceVolConvention::AmericanQuoting)
      .value("EUROPEAN_EQUIVALENT", QuoteSliceVolConvention::EuropeanEquivalent)
      .value("FITTED_CURVE", QuoteSliceVolConvention::FittedCurve)
      .value("REIMPLIED_REPAIRED", QuoteSliceVolConvention::ReimpliedRepaired);
  py::enum_<QuoteSliceFloorPolicy>(m, "QuoteSliceFloorPolicy")
      .value("LEGACY", QuoteSliceFloorPolicy::Legacy)
      .value("PRICE_ONLY", QuoteSliceFloorPolicy::PriceOnly);
  py::enum_<QuoteSliceCarryConvention>(m, "QuoteSliceCarryConvention")
      .value("SUPPLIED", QuoteSliceCarryConvention::Supplied)
      .value("FORWARD_PRESERVING", QuoteSliceCarryConvention::ForwardPreserving);
  py::enum_<QuoteSliceSpotRule>(m, "QuoteSliceSpotRule")
      .value("OWN", QuoteSliceSpotRule::Own)
      .value("NEAREST", QuoteSliceSpotRule::Nearest)
      .value("MEDIAN", QuoteSliceSpotRule::Median);
  py::enum_<QuoteSliceModelKind>(m, "QuoteSliceModelKind")
      .value("ANDERSEN_LAKE", QuoteSliceModelKind::AndersenLake)
      .value("DISCRETE_DIVIDEND_LATTICE", QuoteSliceModelKind::DiscreteDividendLattice);
  // Lane PY (2026-10-23): the surface grade an archived slice carries, and lane C3's
  // served call floor.
  py::enum_<QuoteSliceGrade>(m, "QuoteSliceGrade")
      .value("A", QuoteSliceGrade::A)
      .value("B", QuoteSliceGrade::B)
      .value("C", QuoteSliceGrade::C)
      .value("REFUSED", QuoteSliceGrade::Refused);
  py::enum_<QuoteSliceCallFloor>(m, "QuoteSliceCallFloor")
      .value("LEGACY", QuoteSliceCallFloor::Legacy)
      .value("DISCOUNTED_INTRINSIC", QuoteSliceCallFloor::DiscountedIntrinsic);
  // The repair / box-certificate sub-config (QuoteSliceRepairConfig) and what its rungs
  // did (QuoteSliceRepairRecord), including lane C4's exact in-box projection.
  py::enum_<QuoteSliceRepairMode>(m, "QuoteSliceRepairMode")
      .value("OFF", QuoteSliceRepairMode::Off)
      .value("CHANNEL", QuoteSliceRepairMode::Channel);
  py::enum_<QuoteSliceCertificate>(m, "QuoteSliceCertificate")
      .value("EXACT", QuoteSliceCertificate::Exact)
      .value("BOX", QuoteSliceCertificate::Box);
  py::enum_<QuoteSliceWingMultSource>(m, "QuoteSliceWingMultSource")
      .value("POLICY", QuoteSliceWingMultSource::Policy)
      .value("MARKET", QuoteSliceWingMultSource::Market);
  py::enum_<QuoteSliceMarketDistance>(m, "QuoteSliceMarketDistance")
      .value("OFF", QuoteSliceMarketDistance::Off)
      .value("GATE", QuoteSliceMarketDistance::Gate)
      .value("REPAIR", QuoteSliceMarketDistance::Repair);
  py::enum_<QuoteSliceRepairOutcome>(m, "QuoteSliceRepairOutcome")
      .value("NOT_ATTEMPTED", QuoteSliceRepairOutcome::NotAttempted)
      .value("APPLIED", QuoteSliceRepairOutcome::Applied)
      .value("NO_MOVE", QuoteSliceRepairOutcome::NoMove)
      .value("INFEASIBLE", QuoteSliceRepairOutcome::Infeasible)
      .value("LEFT_CHANNEL", QuoteSliceRepairOutcome::LeftChannel)
      .value("NODE_BUDGET", QuoteSliceRepairOutcome::NodeBudget)
      .value("VOL_BUDGET", QuoteSliceRepairOutcome::VolBudget)
      .value("NOT_CERTIFIED", QuoteSliceRepairOutcome::NotCertified)
      .value("NON_CONVERGED", QuoteSliceRepairOutcome::NonConverged);
  py::enum_<QuoteSliceInboxOutcome>(m, "QuoteSliceInboxOutcome")
      .value("NOT_ATTEMPTED", QuoteSliceInboxOutcome::NotAttempted)
      .value("APPLIED", QuoteSliceInboxOutcome::Applied)
      .value("INFEASIBLE_DATA", QuoteSliceInboxOutcome::InfeasibleData)
      .value("NON_CONVERGED", QuoteSliceInboxOutcome::NonConverged)
      .value("NOT_CERTIFIED", QuoteSliceInboxOutcome::NotCertified)
      .value("INVALID_INPUT", QuoteSliceInboxOutcome::InvalidInput);
  py::enum_<QuoteSliceInboxConflict>(m, "QuoteSliceInboxConflict")
      .value("NONE", QuoteSliceInboxConflict::None)
      .value("EMPTY_BOX", QuoteSliceInboxConflict::EmptyBox)
      .value("NO_CONVEX_CURVE", QuoteSliceInboxConflict::NoConvexCurve)
      .value("AFFINE_BOX", QuoteSliceInboxConflict::AffineBox)
      .value("COUPLED", QuoteSliceInboxConflict::Coupled);
  py::enum_<QuoteSliceBoxRefusal>(m, "QuoteSliceBoxRefusal")
      .value("NONE", QuoteSliceBoxRefusal::None)
      .value("EXERCISE_BOUNDS", QuoteSliceBoxRefusal::ExerciseBounds)
      .value("NO_CONVEX_CURVE", QuoteSliceBoxRefusal::NoConvexCurve)
      .value("NOT_SETTLED", QuoteSliceBoxRefusal::NotSettled)
      .value("CERTIFICATE_REFUSED", QuoteSliceBoxRefusal::CertificateRefused)
      .value("OTHER", QuoteSliceBoxRefusal::Other);

  // Reflection (common.hpp): every quote-slice enum with a C++ to_string.
  atxvol::python::register_enum_names<QuoteSliceRepairOutcome>("QuoteSliceRepairOutcome");
  atxvol::python::register_enum_names<QuoteSliceInboxOutcome>("QuoteSliceInboxOutcome");
  atxvol::python::register_enum_names<QuoteSliceInboxConflict>("QuoteSliceInboxConflict");
  atxvol::python::register_enum_names<QuoteSliceBoxRefusal>("QuoteSliceBoxRefusal");
  namespace pyx = atxvol::python;
  pyx::register_enum_names<QuoteSliceDividendModel>("QuoteSliceDividendModel");
  pyx::register_enum_names<QuoteSliceQuoteStatus>("QuoteSliceQuoteStatus");
  pyx::register_enum_names<QuoteSliceEvaluationStatus>("QuoteSliceEvaluationStatus");
  pyx::register_enum_names<QuoteSliceFitStatus>("QuoteSliceFitStatus");
  pyx::register_enum_names<QuoteSlicePriceOnlyReason>("QuoteSlicePriceOnlyReason");
  pyx::register_enum_names<QuoteSliceVolConvention>("QuoteSliceVolConvention");
  pyx::register_enum_names<QuoteSliceFloorPolicy>("QuoteSliceFloorPolicy");
  pyx::register_enum_names<QuoteSliceCarryConvention>("QuoteSliceCarryConvention");
  pyx::register_enum_names<QuoteSliceSpotRule>("QuoteSliceSpotRule");
  pyx::register_enum_names<QuoteSliceModelKind>("QuoteSliceModelKind");
  pyx::register_enum_names<QuoteSliceGrade>("QuoteSliceGrade");
  pyx::register_enum_names<QuoteSliceCallFloor>("QuoteSliceCallFloor");
}

void bind_inputs(py::module_ &m) {
  py::class_<QuoteSliceMarket>(m, "QuoteSliceMarket")
      .def(py::init([](double spot, double variance_years, double calendar_years, double rate,
                       double continuous_yield, double dividend_pv) {
             return QuoteSliceMarket{spot, variance_years, calendar_years,
                                     rate, continuous_yield, dividend_pv};
           }),
           py::arg("spot") = 0.0, py::arg("variance_years") = 0.0,
           py::arg("calendar_years") = 0.0, py::arg("rate") = 0.0,
           py::arg("continuous_yield") = 0.0, py::arg("dividend_pv") = 0.0)
      .def_readwrite("spot", &QuoteSliceMarket::spot)
      .def_readwrite("variance_years", &QuoteSliceMarket::variance_years)
      .def_readwrite("calendar_years", &QuoteSliceMarket::calendar_years)
      .def_readwrite("rate", &QuoteSliceMarket::rate)
      .def_readwrite("continuous_yield", &QuoteSliceMarket::continuous_yield)
      .def_readwrite("dividend_pv", &QuoteSliceMarket::dividend_pv);

  py::class_<QuoteSliceQuote>(m, "QuoteSliceQuote")
      .def(py::init([](double strike, Side side, double bid, double ask,
                       std::optional<double> continuous_yield) {
             return QuoteSliceQuote{strike, side, bid, ask, continuous_yield};
           }),
           py::arg("strike"), py::arg("side"), py::arg("bid"), py::arg("ask"),
           py::arg("continuous_yield") = py::none())
      .def_readwrite("strike", &QuoteSliceQuote::strike)
      .def_readwrite("side", &QuoteSliceQuote::side)
      .def_readwrite("bid", &QuoteSliceQuote::bid)
      .def_readwrite("ask", &QuoteSliceQuote::ask)
      .def_readwrite("continuous_yield", &QuoteSliceQuote::continuous_yield);

  py::class_<QuoteSliceCallFloorLine>(m, "QuoteSliceCallFloorLine")
      .def(py::init([](double calendar_years, double spot, double discount) {
             return QuoteSliceCallFloorLine{calendar_years, spot, discount};
           }),
           py::arg("calendar_years") = 0.0, py::arg("spot") = 0.0, py::arg("discount") = 1.0)
      .def_readwrite("calendar_years", &QuoteSliceCallFloorLine::calendar_years)
      .def_readwrite("spot", &QuoteSliceCallFloorLine::spot)
      .def_readwrite("discount", &QuoteSliceCallFloorLine::discount);

  py::class_<QuoteSliceRepairConfig>(m, "QuoteSliceRepairConfig")
      .def(py::init<>())
      .def_readwrite("mode", &QuoteSliceRepairConfig::mode)
      .def_readwrite("max_nodes", &QuoteSliceRepairConfig::max_nodes)
      .def_readwrite("max_vol_move", &QuoteSliceRepairConfig::max_vol_move)
      .def_readwrite("immaterial_price_move", &QuoteSliceRepairConfig::immaterial_price_move)
      .def_readwrite("certificate", &QuoteSliceRepairConfig::certificate)
      .def_readwrite("box_half_ticks", &QuoteSliceRepairConfig::box_half_ticks)
      .def_readwrite("box_tick", &QuoteSliceRepairConfig::box_tick)
      // Lane C4 (opt-in, inert under certificate EXACT): the exact in-box projection rung.
      .def_readwrite("inbox_projection", &QuoteSliceRepairConfig::inbox_projection)
      .def_readwrite("wing_mult_source", &QuoteSliceRepairConfig::wing_mult_source)
      .def_readwrite("market_distance", &QuoteSliceRepairConfig::market_distance)
      .def_readwrite("market_distance_spreads", &QuoteSliceRepairConfig::market_distance_spreads)
      .def_readwrite("joint_max_sweeps", &QuoteSliceRepairConfig::joint_max_sweeps)
      .def_readwrite("calendar_parity_clamp", &QuoteSliceRepairConfig::calendar_parity_clamp);
  m.def(
      "validate_quote_slice_repair",
      [](const QuoteSliceRepairConfig &c) { atxvol::python::unwrap(validate_quote_slice_repair(c)); },
      py::arg("config"), "Raises AtxError(INVALID_ARGUMENT) for an unknown mode or a bad bound.");

  py::class_<QuoteSliceConfig>(m, "QuoteSliceConfig")
      .def(py::init<>())
      // The repair / box-certificate sub-config: a reference into this config
      // (def_readwrite's reference_internal), so `config.repair.inbox_projection = True` sticks.
      .def_readwrite("repair", &QuoteSliceConfig::repair)
      // Lane C3's served call floor (opt-in): the mode and the shared lines.
      .def_property(
          "call_floor_mode", [](const QuoteSliceConfig &c) { return c.call_floor.mode; },
          [](QuoteSliceConfig &c, QuoteSliceCallFloor mode) { c.call_floor.mode = mode; })
      .def_property(
          "call_floor_lines", [](const QuoteSliceConfig &c) { return c.call_floor.lines; },
          [](QuoteSliceConfig &c, std::vector<QuoteSliceCallFloorLine> lines) {
            c.call_floor.lines = std::move(lines);
          })
      .def_readwrite("dividend_model", &QuoteSliceConfig::dividend_model)
      .def_readwrite("floor_policy", &QuoteSliceConfig::floor_policy)
      .def_readwrite("min_quotes", &QuoteSliceConfig::min_quotes)
      .def_readwrite("atm_log_band", &QuoteSliceConfig::atm_log_band)
      .def_readwrite("spread_floor", &QuoteSliceConfig::spread_floor)
      .def_readwrite("min_time_value", &QuoteSliceConfig::min_time_value)
      .def_readwrite("iv_tolerance", &QuoteSliceConfig::iv_tolerance);

  py::class_<QuoteSliceSnapshotInfo>(m, "QuoteSliceSnapshotInfo")
      .def(py::init<>())
      .def_readwrite("quote_ns", &QuoteSliceSnapshotInfo::quote_ns)
      .def_readwrite("underlying_ns", &QuoteSliceSnapshotInfo::underlying_ns)
      .def_readwrite("spot_version", &QuoteSliceSnapshotInfo::spot_version)
      .def_readwrite("spot_rule", &QuoteSliceSnapshotInfo::spot_rule)
      .def_readwrite("carry_version", &QuoteSliceSnapshotInfo::carry_version)
      .def_readwrite("carry_convention", &QuoteSliceSnapshotInfo::carry_convention)
      .def_readwrite("raw_spot", &QuoteSliceSnapshotInfo::raw_spot)
      .def_readwrite("spot_stale", &QuoteSliceSnapshotInfo::spot_stale);
}

void bind_results(py::module_ &m) {
  py::class_<QuoteSliceRow>(m, "QuoteSliceRow")
      .def_readonly("quote_status", &QuoteSliceRow::quote_status)
      .def_readonly("evaluation_status", &QuoteSliceRow::evaluation_status)
      .def_readonly("admitted", &QuoteSliceRow::admitted)
      .def_readonly("market_iv", &QuoteSliceRow::market_iv)
      .def_readonly("fitted_iv", &QuoteSliceRow::fitted_iv)
      .def_readonly("fitted_price", &QuoteSliceRow::fitted_price)
      .def_readonly("spread_vol", &QuoteSliceRow::spread_vol)
      .def_readonly("weight_w", &QuoteSliceRow::weight_w)
      .def_readonly("weight_capped", &QuoteSliceRow::weight_capped)
      .def_readonly("price_only_reason", &QuoteSliceRow::price_only_reason)
      .def_readonly("iv_lower", &QuoteSliceRow::iv_lower)
      .def_readonly("iv_upper", &QuoteSliceRow::iv_upper)
      // Lane K4b: a bound (USD) on |grid price - the fit's accurate American model| at the
      // row's certification-grid node; NaN off the native Family grid route.
      .def_readonly("grid_bound_usd", &QuoteSliceRow::grid_bound_usd)
      // The convention of fitted_iv (vol_convention(row)): NONE unless the row is SUCCESS.
      .def_property_readonly("vol_convention",
                             [](const QuoteSliceRow &row) { return vol_convention(row); });

  py::class_<QuoteSliceFit>(m, "QuoteSliceFit")
      .def_readonly("fit_status", &QuoteSliceFit::fit_status)
      .def_readonly("fit_error", &QuoteSliceFit::fit_error)
      .def_readonly("rows", &QuoteSliceFit::rows)
      .def_readonly("market", &QuoteSliceFit::market)
      .def_readonly("dividend_model", &QuoteSliceFit::dividend_model)
      .def_readonly("floor_policy", &QuoteSliceFit::floor_policy)
      .def_readonly("snapshot", &QuoteSliceFit::snapshot)
      .def_readonly("moved_nodes", &QuoteSliceFit::moved_nodes)
      .def_readonly("price_only_nodes", &QuoteSliceFit::price_only_nodes)
      // Lane C2: node sides withheld from the served representation (1 call, 2 put).
      .def_readonly("withheld_nodes", &QuoteSliceFit::withheld_nodes)
      .def_readonly("calendar_parity_clamp", &QuoteSliceFit::calendar_parity_clamp)
      .def_property_readonly("carry_curve",
                             [](const QuoteSliceFit &fit) {
                               std::vector<std::pair<double, double>> out;
                               for (const QuoteSliceCarryPoint &p : fit.carry_curve) {
                                 out.emplace_back(p.strike, p.continuous_yield);
                               }
                               return out;
                             })
      // Lane H1: the serving record certify_and_serve attached (None when absent).
      // SAFETY: the record is immutable and only its const members are bound; the cast
      // matches the holder type serve.cpp registers (std::shared_ptr<SliceServeRecord>).
      .def_property_readonly("serve_record",
                             [](const QuoteSliceFit &fit) {
                               return std::const_pointer_cast<SliceServeRecord>(fit.serve_record);
                             })
      .def_property_readonly("call_floor_lines",
                             [](const QuoteSliceFit &fit) {
                               return fit.diagnostics.call_floor_lines;
                             })
      .def_property_readonly("call_floor_lifted",
                             [](const QuoteSliceFit &fit) {
                               return fit.diagnostics.call_floor_lifted;
                             })
      .def_property_readonly("call_floor_refused",
                             [](const QuoteSliceFit &fit) {
                               return fit.diagnostics.call_floor_refused;
                             })
      // Copies of QuoteSliceDiagnostics::repair / ::box (the latter carries lane C4's
      // in-box projection record as `.inbox`).
      .def_property_readonly("repair_record",
                             [](const QuoteSliceFit &fit) { return fit.diagnostics.repair; })
      .def_property_readonly("box_record",
                             [](const QuoteSliceFit &fit) { return fit.diagnostics.box; })
      .def_property_readonly("node_strikes", &node_strikes)
      .def_property_readonly("model_kind",
                             [](const QuoteSliceFit &fit) { return model_kind(fit); })
      .def_property_readonly("forward",
                             [](const QuoteSliceFit &fit) { return fit.diagnostics.forward; });

  py::class_<QuoteSliceValue>(m, "QuoteSliceValue")
      .def_readonly("volatility", &QuoteSliceValue::volatility)
      .def_readonly("price", &QuoteSliceValue::price);

  py::class_<QuoteSliceQuery>(m, "QuoteSliceQuery")
      .def_readonly("status", &QuoteSliceQuery::status)
      .def_readonly("reason", &QuoteSliceQuery::reason)
      .def_readonly("vol_convention", &QuoteSliceQuery::vol_convention)
      .def_readonly("price", &QuoteSliceQuery::price)
      .def_readonly("volatility", &QuoteSliceQuery::volatility)
      .def_readonly("iv_lower", &QuoteSliceQuery::iv_lower)
      .def_readonly("iv_upper", &QuoteSliceQuery::iv_upper);

  py::class_<QuoteSliceSpotRebase>(m, "QuoteSliceSpotRebase")
      .def_readonly("market", &QuoteSliceSpotRebase::market)
      .def_readonly("yield_shift", &QuoteSliceSpotRebase::yield_shift)
      .def_readonly("forward", &QuoteSliceSpotRebase::forward);

  // What the repair rungs did: QuoteSliceFit.repair_record (channel repair) and
  // QuoteSliceFit.box_record (box certificate, with lane C4's in-box projection `inbox`).
  py::class_<QuoteSliceInboxRecord>(m, "QuoteSliceInboxRecord")
      .def_readonly("outcome", &QuoteSliceInboxRecord::outcome)
      .def_readonly("box_refusal", &QuoteSliceInboxRecord::box_refusal)
      .def_readonly("conflict", &QuoteSliceInboxRecord::conflict)
      .def_readonly("conflict_side", &QuoteSliceInboxRecord::conflict_side)
      .def_readonly("conflict_node", &QuoteSliceInboxRecord::conflict_node)
      .def_readonly("conflict_strike", &QuoteSliceInboxRecord::conflict_strike)
      .def_readonly("conflict_excess", &QuoteSliceInboxRecord::conflict_excess)
      .def_readonly("free_sides", &QuoteSliceInboxRecord::free_sides)
      .def_readonly("fitted_outside", &QuoteSliceInboxRecord::fitted_outside)
      .def_readonly("constraints", &QuoteSliceInboxRecord::constraints)
      .def_readonly("phase1_steps", &QuoteSliceInboxRecord::phase1_steps)
      .def_readonly("phase2_steps", &QuoteSliceInboxRecord::phase2_steps)
      .def_readonly("max_move_half_spreads", &QuoteSliceInboxRecord::max_move_half_spreads)
      .def_readonly("max_mid_half_spreads", &QuoteSliceInboxRecord::max_mid_half_spreads)
      .def_readonly("elapsed_us", &QuoteSliceInboxRecord::elapsed_us);
  py::class_<QuoteSliceRepairRecord>(m, "QuoteSliceRepairRecord")
      .def_readonly("outcome", &QuoteSliceRepairRecord::outcome)
      .def_readonly("nodes", &QuoteSliceRepairRecord::nodes)
      .def_readonly("material_nodes", &QuoteSliceRepairRecord::material_nodes)
      .def_readonly("max_price_move", &QuoteSliceRepairRecord::max_price_move)
      .def_readonly("max_vol_move", &QuoteSliceRepairRecord::max_vol_move)
      .def_readonly("max_unquoted_vol_move", &QuoteSliceRepairRecord::max_unquoted_vol_move)
      .def_readonly("unidentified", &QuoteSliceRepairRecord::unidentified)
      .def_readonly("sweeps", &QuoteSliceRepairRecord::sweeps)
      // JointConflictKind has no binding: the kind is its integer value.
      .def_property_readonly("conflict",
                             [](const QuoteSliceRepairRecord &r) {
                               py::dict d;
                               d["kind"] = static_cast<int>(r.conflict.kind);
                               d["slice"] = r.conflict.slice;
                               d["node"] = r.conflict.node;
                               d["other"] = r.conflict.other;
                               d["strike"] = r.conflict.strike;
                               d["excess"] = r.conflict.excess;
                               return d;
                             })
      .def_readonly("inbox", &QuoteSliceRepairRecord::inbox);

  // Lane C2: a withheld (side, node segment) of the served representation.
  py::class_<QuoteSliceGap>(m, "QuoteSliceGap")
      .def_readonly("side", &QuoteSliceGap::side)
      .def_readonly("lo", &QuoteSliceGap::lo)
      .def_readonly("hi", &QuoteSliceGap::hi)
      .def_readonly("lo_open", &QuoteSliceGap::lo_open)
      .def_readonly("hi_open", &QuoteSliceGap::hi_open);
}

} // namespace

void bind_quote_slice(py::module_ &m) {
  bind_enums(m);
  bind_inputs(m);
  bind_results(m);

  m.def(
      "fit_quote_slice",
      [](const QuoteSliceMarket &market, const std::vector<QuoteSliceQuote> &quotes,
         const QuoteSliceConfig &config, const DividendEvents &dividends) {
        const std::vector<CashDividend> events = dividends_of(dividends);
        py::gil_scoped_release release;
        return atxvol::python::unwrap(
            fit_quote_slice(market, std::span<const QuoteSliceQuote>{quotes}, config,
                            std::span<const CashDividend>{events}));
      },
      py::arg("market"), py::arg("quotes"), py::arg("config") = QuoteSliceConfig{},
      py::arg("dividends") = DividendEvents{});
  // The typed answer: an unidentified volatility is a PRICE_ONLY answer with its price.
  m.def(
      "query_quote_slice",
      [](const QuoteSliceFit &fit, double strike, Side side) {
        return atxvol::python::unwrap(query_quote_slice(fit, strike, side));
      },
      py::arg("fit"), py::arg("strike"), py::arg("side"));
  // Raises AtxError (OUT_OF_RANGE) wherever query_quote_slice answers PRICE_ONLY.
  m.def(
      "evaluate_quote_slice",
      [](const QuoteSliceFit &fit, double strike, Side side) {
        return atxvol::python::unwrap(evaluate_quote_slice(fit, strike, side));
      },
      py::arg("fit"), py::arg("strike"), py::arg("side"));
  m.def(
      "rebase_quote_slice_spot",
      [](const QuoteSliceMarket &market, double snapshot_spot, QuoteSliceDividendModel model,
         const DividendEvents &dividends) {
        const std::vector<CashDividend> events = dividends_of(dividends);
        return atxvol::python::unwrap(rebase_quote_slice_spot(
            market, snapshot_spot, model, std::span<const CashDividend>{events}));
      },
      py::arg("market"), py::arg("snapshot_spot"), py::arg("model"),
      py::arg("dividends") = DividendEvents{});
  // Lane C2: the served price (None outside the support: withheld node sides and the
  // strikes between them and their neighbours, or outside the certified domain).
  m.def(
      "served_quote_slice_price",
      [](const QuoteSliceFit &fit, double strike, Side side) {
        return served_quote_slice_price(fit, strike, side);
      },
      py::arg("fit"), py::arg("strike"), py::arg("side"));
  m.def(
      "quote_slice_gaps", [](const QuoteSliceFit &fit) { return quote_slice_gaps(fit); },
      py::arg("fit"), "The withheld (side, node segment) gaps of the served representation.");
}
