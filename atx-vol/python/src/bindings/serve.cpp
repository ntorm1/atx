// Serving records and the live serve book (lanes H0-A / H1, r6; fitting/
// slice_serve_record.hpp + fitting/serve_book.hpp), bound read-first for research use
// (lane PY, 2026-10-23).
//
// What Python gets: build_serve_record(fit) -> SliceServeRecord (immutable; serve() /
// implied_vol() with typed ServeStatus / ServeRoute answers and the error-statement class
// of every number), the ServeBook publication API (publish, transactions, readers) and
// ServeBookReader.serve_chain, which returns numpy columns plus each contract's
// ServedChainStatement (status, route, tier, certificate class + USD, sampled flag,
// price-only reason + IV interval).
//
// Not bound: the book's async rebuild callback (ServeRebuildFn). A Python callable run on
// the book's std::jthread workers would need the GIL on every rebuild; a book built here
// has no rebuild function, so request_rebuild() answers NO_REBUILD and readers still get
// the typed Refit / Rebuild routes.
//
// Holders: records and versions are immutable C++ objects shared by std::shared_ptr<const
// T>. pybind11 registers them with a std::shared_ptr<T> holder; every handover goes
// through std::const_pointer_cast and only const members are bound (SAFETY: nothing a
// Python caller can reach mutates a record or a version).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/serve_book.hpp"
#include "atx/vol/api/fitting/slice_serve_record.hpp"
#include "atx/vol/api/pricing/chain_tick.hpp"
#include "common.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;
using atxvol::python::F64Column;

namespace {

using RecordPtr = std::shared_ptr<SliceServeRecord>;
using VersionPtr = std::shared_ptr<ServeBookVersion>;

RecordPtr mutable_ptr(const std::shared_ptr<const SliceServeRecord> &p) {
  return std::const_pointer_cast<SliceServeRecord>(p); // SAFETY: see the file comment
}

py::bytes digest_bytes(const ServeDigest &d) {
  return py::bytes(reinterpret_cast<const char *>(d.data()), d.size()); // SAFETY: byte read
}

py::list reasons_list(const ServeGreekReasons &r) {
  py::list out;
  for (const ServeGreekReason g : r) out.append(py::cast(g));
  return out;
}

py::dict convention_dict(const ServeGreekConvention &c) {
  py::dict d;
  d["sticky_strike"] = c.sticky_strike;
  d["model"] = c.model;
  d["theta_pde_identity"] = c.theta_pde_identity;
  d["surface_version"] = c.surface_version;
  return d;
}

void bind_enums(py::module_ &m) {
  namespace pyx = atxvol::python;
  py::enum_<ServeTier>(m, "ServeTier")
      .value("LADDER", ServeTier::Ladder)
      .value("FAMILY", ServeTier::Family)
      .value("CERTIFIED", ServeTier::Certified)
      .value("COLD", ServeTier::Cold);
  pyx::register_enum_names<ServeTier>("ServeTier");
  py::enum_<ServeStatus>(m, "ServeStatus")
      .value("OK", ServeStatus::Ok)
      .value("EXERCISED", ServeStatus::Exercised)
      .value("PRICE_ONLY", ServeStatus::PriceOnly)
      .value("OUT_OF_DOMAIN", ServeStatus::OutOfDomain)
      .value("SIGMA_OUT_OF_BOX", ServeStatus::SigmaOutOfBox)
      .value("TIER_UNAVAILABLE", ServeStatus::TierUnavailable)
      .value("STALE", ServeStatus::Stale)
      .value("NO_CONVERGENCE", ServeStatus::NoConvergence)
      .value("NON_MONOTONE_BAND", ServeStatus::NonMonotoneBand)
      .value("UNCONFIRMED", ServeStatus::Unconfirmed)
      .value("INVALID_ARGUMENT", ServeStatus::InvalidArgument)
      .value("BUDGET_EXHAUSTED", ServeStatus::BudgetExhausted)
      .value("INVALIDATED", ServeStatus::Invalidated);
  pyx::register_enum_names<ServeStatus>("ServeStatus");
  py::enum_<ServeRoute>(m, "ServeRoute")
      .value("NONE", ServeRoute::None)
      .value("COLD_PATH", ServeRoute::ColdPath)
      .value("REBUILD", ServeRoute::Rebuild)
      .value("REFIT", ServeRoute::Refit);
  pyx::register_enum_names<ServeRoute>("ServeRoute");
  py::enum_<ServePriceSource>(m, "ServePriceSource")
      .value("NONE", ServePriceSource::None)
      .value("SLICE_NODE", ServePriceSource::SliceNode)
      .value("SLICE_PIECEWISE_LINEAR", ServePriceSource::SlicePiecewiseLinear)
      .value("LADDER", ServePriceSource::Ladder)
      .value("FAMILY", ServePriceSource::Family)
      .value("FAMILY_FAST_CONFIRM", ServePriceSource::FamilyFastConfirm);
  py::enum_<ServeGreekSource>(m, "ServeGreekSource")
      .value("NONE", ServeGreekSource::None)
      .value("LADDER", ServeGreekSource::Ladder)
      .value("FAMILY", ServeGreekSource::Family);
  py::enum_<ServeBoundClass>(m, "ServeBoundClass")
      .value("NONE", ServeBoundClass::None)
      .value("EXACT", ServeBoundClass::Exact)
      .value("REFINEMENT_VALIDATED", ServeBoundClass::RefinementValidated)
      .value("SAMPLED", ServeBoundClass::Sampled)
      .value("KERNEL_TESTED", ServeBoundClass::KernelTested);
  pyx::register_enum_names<ServeBoundClass>("ServeBoundClass");
  py::enum_<ServeGreek>(m, "ServeGreek")
      .value("DELTA", ServeGreek::Delta)
      .value("GAMMA", ServeGreek::Gamma)
      .value("VEGA", ServeGreek::Vega)
      .value("THETA", ServeGreek::Theta)
      .value("RHO", ServeGreek::Rho)
      .value("PHI", ServeGreek::Phi);
  py::enum_<ServeGreekReason>(m, "ServeGreekReason")
      .value("NOT_REQUESTED", ServeGreekReason::NotRequested)
      .value("SERVED", ServeGreekReason::Served)
      .value("EXERCISED", ServeGreekReason::Exercised)
      .value("REFUSED", ServeGreekReason::Refused)
      .value("NO_CARRY_BUMPS", ServeGreekReason::NoCarryBumps)
      .value("CARRY_BUMP_REFUSED", ServeGreekReason::CarryBumpRefused)
      .value("NO_Q_AXIS", ServeGreekReason::NoQAxis)
      .value("NEAR_NODE_DEGENERATE", ServeGreekReason::NearNodeDegenerate)
      .value("CONVENTION_UNSUPPORTED", ServeGreekReason::ConventionUnsupported)
      .value("NEAR_EXERCISE_EDGE", ServeGreekReason::NearExerciseEdge)
      .value("BUNDLE_UNAVAILABLE", ServeGreekReason::BundleUnavailable)
      .value("TIER_HAS_NO_GREEK", ServeGreekReason::TierHasNoGreek)
      .value("NO_MAP", ServeGreekReason::NoMap);
  pyx::register_enum_names<ServeGreekReason>("ServeGreekReason");
  py::enum_<ServeGreekModel>(m, "ServeGreekModel")
      .value("NONE", ServeGreekModel::None)
      .value("SIGMA_Q_FAMILY", ServeGreekModel::SigmaQFamily)
      .value("NODE_LADDER", ServeGreekModel::NodeLadder);
  py::enum_<ServeAcceptanceKind>(m, "ServeAcceptanceKind")
      .value("NONE", ServeAcceptanceKind::None)
      .value("CONFIRMED", ServeAcceptanceKind::Confirmed)
      .value("SKIPPED", ServeAcceptanceKind::Skipped)
      .value("PRICE_ONLY", ServeAcceptanceKind::PriceOnly)
      .value("INTERVAL", ServeAcceptanceKind::Interval)
      .value("LADDER_CERTIFICATE", ServeAcceptanceKind::LadderCertificate);
  py::enum_<ServeAcceptanceReason>(m, "ServeAcceptanceReason")
      .value("NONE", ServeAcceptanceReason::None)
      .value("FAMILY_ENCLOSURE", ServeAcceptanceReason::FamilyEnclosure)
      .value("CONFIRM_AND_FAMILY", ServeAcceptanceReason::ConfirmAndFamily)
      .value("CONFIRM_BOTH_ENDS", ServeAcceptanceReason::ConfirmBothEnds)
      .value("LOW_VEGA", ServeAcceptanceReason::LowVega)
      .value("BUDGET_EXHAUSTED", ServeAcceptanceReason::BudgetExhausted)
      .value("NEWTON_CONTRACT", ServeAcceptanceReason::NewtonContract)
      .value("LEGACY_BAND_GATE", ServeAcceptanceReason::LegacyBandGate)
      .value("LEGACY_NEVER", ServeAcceptanceReason::LegacyNever)
      .value("FITTED_CURVE", ServeAcceptanceReason::FittedCurve);
  py::enum_<ServeErrorClass>(m, "ServeErrorClass")
      .value("UNCOVERED", ServeErrorClass::Uncovered)
      .value("REFINEMENT_VALIDATED", ServeErrorClass::RefinementValidated);
  py::enum_<ServeUnsupportedReason>(m, "ServeUnsupportedReason")
      .value("NONE", ServeUnsupportedReason::None)
      .value("DATED_CASH", ServeUnsupportedReason::DatedCash)
      .value("BELOW_FAMILY_TAU", ServeUnsupportedReason::BelowFamilyTau)
      .value("FAMILY_REFUSED", ServeUnsupportedReason::FamilyRefused)
      .value("FAMILIES_NOT_BUILT", ServeUnsupportedReason::FamiliesNotBuilt)
      .value("NO_SERVED_VOLATILITY", ServeUnsupportedReason::NoServedVolatility)
      .value("HANDOFF_MISMATCH", ServeUnsupportedReason::HandoffMismatch);
  pyx::register_enum_names<ServeUnsupportedReason>("ServeUnsupportedReason");
  py::enum_<ServeCashDividends>(m, "ServeCashDividends")
      .value("NOT_PRESENT", ServeCashDividends::NotPresent)
      .value("UNSUPPORTED_IN_RECORD", ServeCashDividends::UnsupportedInRecord);
  py::enum_<ServeFamilyOrigin>(m, "ServeFamilyOrigin")
      .value("NONE", ServeFamilyOrigin::None)
      .value("BUILT", ServeFamilyOrigin::Built)
      .value("HANDED", ServeFamilyOrigin::Handed)
      .value("SUPPLIED", ServeFamilyOrigin::Supplied);
  py::enum_<ServeFamilyMode>(m, "ServeFamilyMode")
      .value("NONE", ServeFamilyMode::None)
      .value("BUILD", ServeFamilyMode::Build);
  py::enum_<ServeRebuildReason>(m, "ServeRebuildReason")
      .value("REQUESTED", ServeRebuildReason::Requested)
      .value("STALE", ServeRebuildReason::Stale)
      .value("INVALIDATED", ServeRebuildReason::Invalidated)
      .value("MISSING", ServeRebuildReason::Missing);
  pyx::register_enum_names<ServeRebuildReason>("ServeRebuildReason");
  py::enum_<ServeEnqueue>(m, "ServeEnqueue")
      .value("QUEUED", ServeEnqueue::Queued)
      .value("DEDUPLICATED", ServeEnqueue::Deduplicated)
      .value("FULL", ServeEnqueue::Full)
      .value("NO_REBUILD", ServeEnqueue::NoRebuild);
  pyx::register_enum_names<ServeEnqueue>("ServeEnqueue");
  py::enum_<TickStatus>(m, "TickStatus")
      .value("OK", TickStatus::Ok)
      .value("EXERCISED", TickStatus::Exercised)
      .value("OUT_OF_WINDOW", TickStatus::OutOfWindow)
      .value("FALLBACK", TickStatus::Fallback)
      .value("UNPRICEABLE", TickStatus::Unpriceable);

  m.attr("SERVE_GREEKS_NONE") = ServeGreekMask::none().bits;
  m.attr("SERVE_GREEKS_FOUR") = ServeGreekMask::four().bits;
  m.attr("SERVE_GREEKS_ALL") = ServeGreekMask::all().bits;
  m.attr("SERVE_EDGE_BAND") = kServeEdgeBand;
  m.attr("SERVE_NODE_LADDER") = kServeNodeLadder;
  m.attr("SERVE_NODE_FAMILY") = kServeNodeFamily;
}

void bind_policy_and_requests(py::module_ &m) {
  py::class_<ServeRecordPolicy>(m, "ServeRecordPolicy")
      .def(py::init<>())
      .def_readwrite("families", &ServeRecordPolicy::families)
      .def_readwrite("ladders", &ServeRecordPolicy::ladders)
      .def_readwrite("sampled_ladders", &ServeRecordPolicy::sampled_ladders)
      .def_readwrite("sigma_lo_mult", &ServeRecordPolicy::sigma_lo_mult)
      .def_readwrite("sigma_hi_mult", &ServeRecordPolicy::sigma_hi_mult)
      .def_readwrite("ladder_spot_nodes", &ServeRecordPolicy::ladder_spot_nodes)
      .def_readwrite("ladder_sigma_nodes", &ServeRecordPolicy::ladder_sigma_nodes)
      .def_readwrite("ladder_window_sigmas", &ServeRecordPolicy::ladder_window_sigmas)
      .def_readwrite("ladder_window_min", &ServeRecordPolicy::ladder_window_min)
      .def_readwrite("ladder_window_max", &ServeRecordPolicy::ladder_window_max)
      .def_readwrite("ladder_sigma_half_width", &ServeRecordPolicy::ladder_sigma_half_width)
      .def_readwrite("ladder_max_cert_usd", &ServeRecordPolicy::ladder_max_cert_usd)
      .def_readwrite("ladder_max_cert_rel", &ServeRecordPolicy::ladder_max_cert_rel)
      .def_readwrite("max_age_ns", &ServeRecordPolicy::max_age_ns)
      .def_readwrite("iv_tolerance", &ServeRecordPolicy::iv_tolerance)
      .def_readwrite("surface_version", &ServeRecordPolicy::surface_version);
  m.def("serve_record_policy_tick", &serve_record_policy_tick,
        "The ladder profile whose statements can accept a tick-rate IV root (lane H1).");
  m.def(
      "validate_serve_record_policy",
      [](const ServeRecordPolicy &p) { atxvol::python::unwrap(validate_serve_record_policy(p)); },
      py::arg("policy"), "Raises AtxError(INVALID_ARGUMENT) for an out-of-range field.");

  py::class_<ServeRequest>(m, "ServeRequest")
      .def(py::init([](double strike, Side side, double spot, ServeTier tier, ServeTier tier_max,
                       std::uint8_t greeks, std::int64_t now_ns, bool family_bound) {
             ServeRequest r;
             r.strike = strike;
             r.side = side;
             r.spot = spot;
             r.tier = tier;
             r.tier_max = tier_max;
             r.greeks = ServeGreekMask{greeks};
             r.now_ns = now_ns;
             r.family_bound = family_bound;
             return r;
           }),
           py::arg("strike"), py::arg("side"), py::arg("spot"),
           py::arg("tier") = ServeTier::Ladder, py::arg("tier_max") = ServeTier::Certified,
           py::arg("greeks") = ServeGreekMask::four().bits, py::arg("now_ns") = 0,
           py::arg("family_bound") = true)
      .def_readwrite("strike", &ServeRequest::strike)
      .def_readwrite("side", &ServeRequest::side)
      .def_readwrite("spot", &ServeRequest::spot)
      .def_readwrite("tier", &ServeRequest::tier)
      .def_readwrite("tier_max", &ServeRequest::tier_max)
      .def_property(
          "greeks", [](const ServeRequest &r) { return r.greeks.bits; },
          [](ServeRequest &r, std::uint8_t bits) { r.greeks = ServeGreekMask{bits}; })
      .def_readwrite("now_ns", &ServeRequest::now_ns)
      .def_readwrite("family_bound", &ServeRequest::family_bound);

  py::class_<ServeIvRequest>(m, "ServeIvRequest")
      .def(py::init([](double strike, Side side, double spot, double target_price,
                       ServeTier tier, ServeTier tier_max, std::int64_t now_ns) {
             ServeIvRequest r;
             r.strike = strike;
             r.side = side;
             r.spot = spot;
             r.target_price = target_price;
             r.tier = tier;
             r.tier_max = tier_max;
             r.now_ns = now_ns;
             return r;
           }),
           py::arg("strike"), py::arg("side"), py::arg("spot"), py::arg("target_price"),
           py::arg("tier") = ServeTier::Ladder, py::arg("tier_max") = ServeTier::Certified,
           py::arg("now_ns") = 0)
      .def_readwrite("strike", &ServeIvRequest::strike)
      .def_readwrite("side", &ServeIvRequest::side)
      .def_readwrite("spot", &ServeIvRequest::spot)
      .def_readwrite("target_price", &ServeIvRequest::target_price)
      .def_readwrite("tier", &ServeIvRequest::tier)
      .def_readwrite("tier_max", &ServeIvRequest::tier_max)
      .def_readwrite("now_ns", &ServeIvRequest::now_ns);
}

void bind_answers(py::module_ &m) {
  py::class_<ServeRootAcceptance>(m, "ServeRootAcceptance")
      .def_readonly("kind", &ServeRootAcceptance::kind)
      .def_readonly("reason", &ServeRootAcceptance::reason)
      .def_readonly("error_class", &ServeRootAcceptance::error_class)
      .def_readonly("bound_iv", &ServeRootAcceptance::bound_iv);

  py::class_<ServeAnswer>(m, "ServeAnswer")
      .def_readonly("price", &ServeAnswer::price)
      .def_readonly("delta", &ServeAnswer::delta)
      .def_readonly("gamma", &ServeAnswer::gamma)
      .def_readonly("vega", &ServeAnswer::vega)
      .def_readonly("theta", &ServeAnswer::theta)
      .def_readonly("rho", &ServeAnswer::rho)
      .def_readonly("phi", &ServeAnswer::phi)
      .def_property_readonly("reason", [](const ServeAnswer &a) { return reasons_list(a.reason); },
                             "Per-Greek reasons, ServeGreek order (delta ... phi).")
      .def_readonly("sigma", &ServeAnswer::sigma)
      .def_readonly("price_map_delta", &ServeAnswer::price_map_delta)
      .def_readonly("certificate_usd", &ServeAnswer::certificate_usd)
      .def_readonly("certificate_class", &ServeAnswer::certificate_class)
      .def_readonly("map_certificate_usd", &ServeAnswer::map_certificate_usd)
      .def_readonly("map_certificate_class", &ServeAnswer::map_certificate_class)
      .def_readonly("sampled", &ServeAnswer::sampled)
      .def_readonly("price_only_reason", &ServeAnswer::price_only_reason)
      .def_readonly("iv_lower", &ServeAnswer::iv_lower)
      .def_readonly("iv_upper", &ServeAnswer::iv_upper)
      .def_readonly("edge_sd", &ServeAnswer::edge_sd)
      .def_readonly("near_edge", &ServeAnswer::near_edge)
      .def_readonly("age_ns", &ServeAnswer::age_ns)
      .def_readonly("tier", &ServeAnswer::tier)
      .def_readonly("status", &ServeAnswer::status)
      .def_readonly("route", &ServeAnswer::route)
      .def_readonly("price_source", &ServeAnswer::price_source)
      .def_readonly("greek_source", &ServeAnswer::greek_source)
      .def_property_readonly("convention",
                             [](const ServeAnswer &a) { return convention_dict(a.convention); });

  py::class_<ServeIvAnswer>(m, "ServeIvAnswer")
      .def_readonly("sigma", &ServeIvAnswer::sigma)
      .def_readonly("price_residual", &ServeIvAnswer::price_residual)
      .def_readonly("newton_distance_vol", &ServeIvAnswer::newton_distance_vol)
      .def_readonly("acceptance", &ServeIvAnswer::acceptance)
      .def_readonly("evaluations", &ServeIvAnswer::evaluations)
      .def_readonly("age_ns", &ServeIvAnswer::age_ns)
      .def_readonly("tier", &ServeIvAnswer::tier)
      .def_readonly("status", &ServeIvAnswer::status)
      .def_readonly("route", &ServeIvAnswer::route);
}

void bind_record(py::module_ &m) {
  py::class_<ServedNodeSide>(m, "ServedNodeSide")
      .def_readonly("price", &ServedNodeSide::price)
      .def_readonly("status", &ServedNodeSide::status)
      .def_readonly("reason", &ServedNodeSide::reason)
      .def_readonly("convention", &ServedNodeSide::convention)
      .def_readonly("sigma", &ServedNodeSide::sigma)
      .def_readonly("iv_lower", &ServedNodeSide::iv_lower)
      .def_readonly("iv_upper", &ServedNodeSide::iv_upper)
      .def_readonly("q", &ServedNodeSide::q)
      .def_readonly("moved", &ServedNodeSide::moved)
      .def_readonly("acceptance", &ServedNodeSide::acceptance)
      .def_readonly("family_class", &ServedNodeSide::family_class)
      .def_readonly("family_price_delta", &ServedNodeSide::family_price_delta)
      .def_readonly("ladder_certificate_usd", &ServedNodeSide::ladder_certificate_usd)
      .def_readonly("ladder_class", &ServedNodeSide::ladder_class)
      .def_readonly("ladder_plane_tail_usd", &ServedNodeSide::ladder_plane_tail_usd)
      .def_readonly("ladder_plane_check_usd", &ServedNodeSide::ladder_plane_check_usd)
      .def_readonly("ladder_family_bound_usd", &ServedNodeSide::ladder_family_bound_usd)
      .def_readonly("tiers", &ServedNodeSide::tiers)
      .def_property_readonly("family_reasons",
                             [](const ServedNodeSide &s) { return reasons_list(s.family_reasons); })
      .def_readonly("edge_spot", &ServedNodeSide::edge_spot);
  py::class_<ServedNode>(m, "ServedNode")
      .def_readonly("strike", &ServedNode::strike)
      .def_readonly("call", &ServedNode::call)
      .def_readonly("put", &ServedNode::put);

  py::class_<ServeSideSupport>(m, "ServeSideSupport")
      .def_readonly("family", &ServeSideSupport::family)
      .def_readonly("origin", &ServeSideSupport::origin)
      .def_readonly("ladders", &ServeSideSupport::ladders)
      .def_readonly("ladders_validated", &ServeSideSupport::ladders_validated)
      .def_readonly("ladders_sampled", &ServeSideSupport::ladders_sampled)
      .def_readonly("ladders_ineligible", &ServeSideSupport::ladders_ineligible)
      .def_readonly("reason", &ServeSideSupport::reason)
      .def_readonly("sigma_lo", &ServeSideSupport::sigma_lo)
      .def_readonly("sigma_hi", &ServeSideSupport::sigma_hi)
      .def_readonly("q_lo", &ServeSideSupport::q_lo)
      .def_readonly("q_hi", &ServeSideSupport::q_hi)
      .def_readonly("family_class", &ServeSideSupport::family_class)
      .def_readonly("envelope_status", &ServeSideSupport::envelope_status)
      .def_readonly("unverified_nodes", &ServeSideSupport::unverified_nodes)
      .def_readonly("audit_probes", &ServeSideSupport::audit_probes)
      .def_readonly("audit_violations", &ServeSideSupport::audit_violations)
      .def_readonly("audit_max_bound_ratio", &ServeSideSupport::audit_max_bound_ratio)
      .def_readonly("family_bound_rel", &ServeSideSupport::family_bound_rel)
      .def_readonly("domain_bound_usd", &ServeSideSupport::domain_bound_usd)
      .def_readonly("between_node_bound_usd", &ServeSideSupport::between_node_bound_usd);
  py::class_<ServeSupport>(m, "ServeSupport")
      .def_property_readonly("tiers",
                             [](const ServeSupport &s) {
                               return std::vector<bool>(s.tiers.begin(), s.tiers.end());
                             })
      .def_readonly("call", &ServeSupport::call)
      .def_readonly("put", &ServeSupport::put)
      .def_readonly("cash_dividends", &ServeSupport::cash_dividends)
      .def_readonly("reason", &ServeSupport::reason)
      .def_readonly("double_continuation", &ServeSupport::double_continuation)
      .def_readonly("tau", &ServeSupport::tau)
      .def_readonly("tau_min_family", &ServeSupport::tau_min_family)
      .def_property_readonly("greeks",
                             [](const ServeSupport &s) {
                               std::vector<std::uint8_t> out;
                               for (const ServeGreekMask g : s.greeks) out.push_back(g.bits);
                               return out;
                             })
      .def_readonly("a0_bundle", &ServeSupport::a0_bundle)
      .def_readonly("k3_envelope", &ServeSupport::k3_envelope);

  py::class_<ServeRecordVersion>(m, "ServeRecordVersion")
      .def_property_readonly("fit_sha256",
                             [](const ServeRecordVersion &v) { return digest_bytes(v.fit_sha256); })
      .def_property_readonly("policy_sha256",
                             [](const ServeRecordVersion &v) { return digest_bytes(v.policy_sha256); })
      .def_property_readonly("input_sha256",
                             [](const ServeRecordVersion &v) { return digest_bytes(v.input_sha256); })
      .def_readonly("build_steady_ns", &ServeRecordVersion::build_steady_ns)
      .def_readonly("build_wall_ns", &ServeRecordVersion::build_wall_ns)
      .def_readonly("as_of_ns", &ServeRecordVersion::as_of_ns);
  py::class_<ServeRecordBytes>(m, "ServeRecordBytes")
      .def_readonly("nodes", &ServeRecordBytes::nodes)
      .def_readonly("ladders", &ServeRecordBytes::ladders)
      .def_readonly("families", &ServeRecordBytes::families)
      .def_readonly("geometries", &ServeRecordBytes::geometries)
      .def_readonly("fixed", &ServeRecordBytes::fixed)
      .def_property_readonly("total", &ServeRecordBytes::total);
  py::class_<ServeBuildClocks>(m, "ServeBuildClocks")
      .def_readonly("identify_us", &ServeBuildClocks::identify_us)
      .def_readonly("family_us", &ServeBuildClocks::family_us)
      .def_readonly("ladder_us", &ServeBuildClocks::ladder_us)
      .def_readonly("audit_us", &ServeBuildClocks::audit_us)
      .def_readonly("digest_us", &ServeBuildClocks::digest_us)
      .def_readonly("total_us", &ServeBuildClocks::total_us)
      .def_readonly("lazy_family_builds", &ServeBuildClocks::lazy_family_builds)
      .def_readonly("family_evaluations", &ServeBuildClocks::family_evaluations)
      .def_readonly("reimplied_nodes", &ServeBuildClocks::reimplied_nodes);

  py::class_<SliceServeRecord, RecordPtr>(
      m, "SliceServeRecord",
      "The immutable serving bundle of one expiry (fitting/slice_serve_record.hpp).")
      .def("nodes",
           [](const SliceServeRecord &r) {
             return std::vector<ServedNode>(r.nodes().begin(), r.nodes().end());
           })
      .def_property_readonly("node_strikes",
                             [](const SliceServeRecord &r) {
                               py::array_t<double> out(static_cast<py::ssize_t>(r.nodes().size()));
                               double *p = out.mutable_data();
                               for (std::size_t i = 0; i < r.nodes().size(); ++i) {
                                 p[i] = r.nodes()[i].strike;
                               }
                               return out;
                             })
      .def_property_readonly("support", &SliceServeRecord::support,
                             py::return_value_policy::copy)
      .def_property_readonly("version", &SliceServeRecord::version, py::return_value_policy::copy)
      .def_property_readonly("bytes", &SliceServeRecord::bytes, py::return_value_policy::copy)
      .def_property_readonly("clocks", &SliceServeRecord::clocks, py::return_value_policy::copy)
      .def_property_readonly("policy", &SliceServeRecord::policy, py::return_value_policy::copy)
      .def_property_readonly("curve",
                             [](const RecordPtr &r) {
                               // Aliasing share: the curve lives inside the record.
                               return atxvol::python::CurveHandle{
                                   std::shared_ptr<const IVolCurve>(r, &r->curve())};
                             })
      .def_property_readonly("spot", &SliceServeRecord::spot)
      .def_property_readonly("tau", &SliceServeRecord::tau)
      .def_property_readonly("rate", &SliceServeRecord::rate)
      .def_property_readonly("strike_lo", &SliceServeRecord::strike_lo)
      .def_property_readonly("strike_hi", &SliceServeRecord::strike_hi)
      .def("serve", &SliceServeRecord::serve, py::arg("request"),
           py::call_guard<py::gil_scoped_release>())
      .def("implied_vol", &SliceServeRecord::implied_vol, py::arg("request"),
           py::call_guard<py::gil_scoped_release>());

  m.def(
      "build_serve_record",
      [](const QuoteSliceFit &fit, const ServeRecordPolicy &policy) {
        Result<std::shared_ptr<const SliceServeRecord>> built = [&] {
          py::gil_scoped_release release;
          return build_serve_record(fit, policy);
        }();
        return mutable_ptr(atxvol::python::unwrap(std::move(built)));
      },
      py::arg("fit"), py::arg("policy") = ServeRecordPolicy{},
      "Build the serving record of a certified fit; AtxError(UNAVAILABLE) without a "
      "certified slice, INVALID_ARGUMENT for an invalid policy.");
}

void bind_book(py::module_ &m) {
  py::class_<ServeBookPolicy>(m, "ServeBookPolicy")
      .def(py::init([](ServeTier primary_tier_max, std::size_t queue_capacity,
                       unsigned build_workers) {
             return ServeBookPolicy{primary_tier_max, queue_capacity, build_workers};
           }),
           py::arg("primary_tier_max") = ServeTier::Family, py::arg("queue_capacity") = 64,
           py::arg("build_workers") = 0)
      .def_readwrite("primary_tier_max", &ServeBookPolicy::primary_tier_max)
      .def_readwrite("queue_capacity", &ServeBookPolicy::queue_capacity)
      .def_readwrite("build_workers", &ServeBookPolicy::build_workers);

  py::class_<ServeBookEntry>(m, "ServeBookEntry")
      .def(py::init([](std::uint64_t expiry_id, const RecordPtr &record, std::int64_t expiry_ns,
                       std::int64_t next_event_ns, std::uint64_t tag) {
             return ServeBookEntry{expiry_id, record, expiry_ns, next_event_ns, tag};
           }),
           // record=None is a key (serve_build stages the record itself).
           py::arg("expiry_id"), py::arg("record") = py::none(), py::arg("expiry_ns") = 0,
           py::arg("next_event_ns") = 0, py::arg("tag") = 0)
      .def_readonly("expiry_id", &ServeBookEntry::expiry_id)
      .def_property_readonly("record", [](const ServeBookEntry &e) { return mutable_ptr(e.record); })
      .def_readonly("expiry_ns", &ServeBookEntry::expiry_ns)
      .def_readonly("next_event_ns", &ServeBookEntry::next_event_ns)
      .def_readonly("tag", &ServeBookEntry::tag);

  py::class_<ServeBookVersion, VersionPtr>(m, "ServeBookVersion")
      .def_readonly("number", &ServeBookVersion::number)
      .def_readonly("published_steady_ns", &ServeBookVersion::published_steady_ns)
      .def_readonly("entries", &ServeBookVersion::entries)
      .def(
          "find",
          [](const ServeBookVersion &v, std::uint64_t expiry_id) -> py::object {
            const ServeBookEntry *e = v.find(expiry_id);
            return e == nullptr ? py::object(py::none()) : py::cast(*e);
          },
          py::arg("expiry_id"));

  py::class_<ServeBookCounters>(m, "ServeBookCounters")
      .def_readonly("published", &ServeBookCounters::published)
      .def_readonly("aborted", &ServeBookCounters::aborted)
      .def_readonly("reclaimed", &ServeBookCounters::reclaimed)
      .def_readonly("retired_held", &ServeBookCounters::retired_held)
      .def_readonly("enqueued", &ServeBookCounters::enqueued)
      .def_readonly("deduplicated", &ServeBookCounters::deduplicated)
      .def_readonly("rejected", &ServeBookCounters::rejected)
      .def_readonly("rebuilt", &ServeBookCounters::rebuilt)
      .def_readonly("rebuild_failed", &ServeBookCounters::rebuild_failed)
      .def_readonly("queue_depth", &ServeBookCounters::queue_depth);

  py::class_<ServeBookTransaction>(m, "ServeBookTransaction",
                                   "Collects puts / retirements; commit() publishes one version.")
      .def(
          "put",
          [](ServeBookTransaction &t, ServeBookEntry entry) {
            atxvol::python::unwrap(t.put(std::move(entry)));
          },
          py::arg("entry"))
      .def("retire", &ServeBookTransaction::retire, py::arg("expiry_id"))
      .def("retire_all", &ServeBookTransaction::retire_all)
      .def("commit",
           [](ServeBookTransaction &t) { return atxvol::python::unwrap(t.commit()); });

  py::class_<ServeBookReader>(m, "ServeBookReader",
                              "One thread's handle on a ServeBook (make one per thread).")
      .def("serve", &ServeBookReader::serve, py::arg("expiry_id"), py::arg("request"))
      .def("implied_vol", &ServeBookReader::implied_vol, py::arg("expiry_id"), py::arg("request"))
      .def(
          "serve_chain",
          [](ServeBookReader &reader, double spot, std::int64_t now_ns,
             const py::array_t<std::uint64_t, py::array::c_style | py::array::forcecast> &expiry_id,
             const F64Column &strike, const py::object &side, ServeTier tier,
             ServeTier tier_max) {
            if (expiry_id.ndim() != 1) throw py::value_error("expiry_id must be one-dimensional");
            const std::size_t n = static_cast<std::size_t>(expiry_id.size());
            const auto k = atxvol::python::column(strike, "strike", n, "expiry_id");
            const std::vector<Side> sides = atxvol::python::side_column(side, n, "expiry_id");
            py::array_t<double> price(static_cast<py::ssize_t>(n)), delta(static_cast<py::ssize_t>(n)),
                gamma(static_cast<py::ssize_t>(n)), vega(static_cast<py::ssize_t>(n)),
                theta(static_cast<py::ssize_t>(n));
            py::array_t<std::uint8_t> tick(static_cast<py::ssize_t>(n));
            std::vector<TickStatus> ticks(n);
            std::vector<ServedChainStatement> statements(n);
            const ChainTickOutputs out{
                {price.mutable_data(), n}, {delta.mutable_data(), n}, {gamma.mutable_data(), n},
                {vega.mutable_data(), n},  {theta.mutable_data(), n}, ticks};
            const ServedChainView chain{{expiry_id.data(), n}, k, sides};
            Status status;
            {
              py::gil_scoped_release release;
              status = reader.serve_chain(spot, now_ns, chain, tier, tier_max, out, statements);
            }
            atxvol::python::unwrap(status);
            std::uint8_t *pt = tick.mutable_data();
            for (std::size_t i = 0; i < n; ++i) pt[i] = static_cast<std::uint8_t>(ticks[i]);
            py::array_t<double> cert(static_cast<py::ssize_t>(n)),
                map_cert(static_cast<py::ssize_t>(n)), iv_lo(static_cast<py::ssize_t>(n)),
                iv_hi(static_cast<py::ssize_t>(n));
            py::array_t<std::uint8_t> st(static_cast<py::ssize_t>(n)), route(static_cast<py::ssize_t>(n)),
                served_tier(static_cast<py::ssize_t>(n)), cls(static_cast<py::ssize_t>(n)),
                map_cls(static_cast<py::ssize_t>(n)), reason(static_cast<py::ssize_t>(n));
            py::array_t<bool> sampled(static_cast<py::ssize_t>(n));
            for (std::size_t i = 0; i < n; ++i) {
              const ServedChainStatement &s = statements[i];
              cert.mutable_data()[i] = s.certificate_usd;
              map_cert.mutable_data()[i] = s.map_certificate_usd;
              iv_lo.mutable_data()[i] = s.iv_lower;
              iv_hi.mutable_data()[i] = s.iv_upper;
              st.mutable_data()[i] = static_cast<std::uint8_t>(s.status);
              route.mutable_data()[i] = static_cast<std::uint8_t>(s.route);
              served_tier.mutable_data()[i] = static_cast<std::uint8_t>(s.tier);
              cls.mutable_data()[i] = static_cast<std::uint8_t>(s.certificate_class);
              map_cls.mutable_data()[i] = static_cast<std::uint8_t>(s.map_certificate_class);
              reason.mutable_data()[i] = static_cast<std::uint8_t>(s.price_only_reason);
              sampled.mutable_data()[i] = s.sampled;
            }
            py::dict d;
            d["price"] = std::move(price);
            d["delta"] = std::move(delta);
            d["gamma"] = std::move(gamma);
            d["vega"] = std::move(vega);
            d["theta"] = std::move(theta);
            d["tick_status"] = std::move(tick);
            d["status"] = std::move(st);
            d["route"] = std::move(route);
            d["tier"] = std::move(served_tier);
            d["certificate_usd"] = std::move(cert);
            d["certificate_class"] = std::move(cls);
            d["map_certificate_usd"] = std::move(map_cert);
            d["map_certificate_class"] = std::move(map_cls);
            d["price_only_reason"] = std::move(reason);
            d["iv_lower"] = std::move(iv_lo);
            d["iv_upper"] = std::move(iv_hi);
            d["sampled"] = std::move(sampled);
            d["version"] = reader.pinned().number;
            return d;
          },
          py::arg("spot"), py::arg("now_ns"), py::arg("expiry_id"), py::arg("strike"),
          py::arg("side"), py::arg("tier") = ServeTier::Ladder,
          py::arg("tier_max") = ServeTier::Family,
          "Every contract from ONE pinned version: numpy columns (price, Greeks, tick_status) "
          "plus each ServedChainStatement field as a column (status / route / tier / "
          "certificate_class / map_certificate_class / price_only_reason hold enum values).")
      .def_property_readonly("pinned_version",
                             [](const ServeBookReader &r) { return r.pinned().number; })
      .def("refresh", &ServeBookReader::refresh);

  py::class_<ServeBook>(m, "ServeBook", "The live, versioned book of serving records.")
      .def(py::init([](const ServeBookPolicy &policy) {
             return std::make_unique<ServeBook>(policy);
           }),
           py::arg("policy") = ServeBookPolicy{ServeTier::Family, 64, 0})
      .def("transaction", &ServeBook::transaction, py::keep_alive<0, 1>(),
           "A transaction (keeps the book alive while it exists).")
      .def(
          "publish_expiry",
          [](ServeBook &b, ServeBookEntry entry) {
            return atxvol::python::unwrap(b.publish_expiry(std::move(entry)));
          },
          py::arg("entry"))
      .def(
          "publish",
          [](ServeBook &b, const std::vector<ServeBookEntry> &entries) {
            return atxvol::python::unwrap(b.publish(entries));
          },
          py::arg("entries"))
      .def_property_readonly("version", &ServeBook::version)
      .def("current",
           [](const ServeBook &b) { return std::const_pointer_cast<ServeBookVersion>(b.current()); })
      .def("reader", &ServeBook::reader)
      .def_property_readonly("policy", &ServeBook::policy, py::return_value_policy::copy)
      .def("request_rebuild", &ServeBook::request_rebuild, py::arg("expiry_id"),
           py::arg("reason") = ServeRebuildReason::Requested)
      .def("run_pending", &ServeBook::run_pending,
           py::arg("max") = static_cast<std::size_t>(-1))
      .def(
          "wait_idle",
          [](ServeBook &b, double timeout_s) {
            py::gil_scoped_release release;
            return b.wait_idle(std::chrono::milliseconds(static_cast<std::int64_t>(timeout_s * 1e3)));
          },
          py::arg("timeout_s"))
      .def("reclaim", &ServeBook::reclaim)
      .def("counters", &ServeBook::counters);

  py::class_<ServeBuildStage>(m, "ServeBuildStage")
      .def_readonly("first_version", &ServeBuildStage::first_version)
      .def_readonly("final_version", &ServeBuildStage::final_version)
      .def_readonly("first_usable_us", &ServeBuildStage::first_usable_us)
      .def_readonly("total_us", &ServeBuildStage::total_us)
      .def_property_readonly("record", [](const ServeBuildStage &s) { return mutable_ptr(s.record); });
  m.def(
      "serve_build",
      [](ServeBook &book, const ServeBookEntry &key, const QuoteSliceFit &fit,
         const ServeRecordPolicy &policy, bool two_phase) {
        Result<ServeBuildStage> stage = [&] {
          py::gil_scoped_release release;
          return serve_build(book, key, fit, policy, two_phase);
        }();
        return atxvol::python::unwrap(std::move(stage));
      },
      py::arg("book"), py::arg("key"), py::arg("fit"), py::arg("policy") = ServeRecordPolicy{},
      py::arg("two_phase") = true,
      "Build a certified fit's record and publish it under key (key.record is ignored).");
}

} // namespace

void bind_serve(py::module_ &m) {
  bind_enums(m);
  bind_policy_and_requests(m);
  bind_answers(m);
  bind_record(m);
  bind_book(m);
}
