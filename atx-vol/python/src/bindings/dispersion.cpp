// Bindings for the two dispersion backtest routes and the run-directory
// readers they share.
//
// The routes differ in where a contract comes from, not in how P&L is folded:
//
//   projection  `run_dispersion_backtest` -> DispersionStrategy. Strikes are
//               resolved ATM-forward on the interpolated surface and the expiry
//               is a projected calendar anchor, so each lot is a synthetic
//               contract repriced at a continuous (K, T) every step.
//   listed      ListedDispersionStrategy over a cold-authored
//               `ListedDispersionSchedule` of real OPRA contracts. Selection is
//               frozen at schedule-build time; the strategy only replays it and
//               requires QueryExecution::ColdReference.
//
// Both feed the same engine, so a run of each over one corpus is directly
// comparable column by column.

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/backtest.hpp"
#include "atx/vol/dispersion.hpp"
#include "atx/vol/dispersion_backtest.hpp"
#include "atx/vol/dispersion_workflow.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/listed_dispersion_strategy.hpp"
#include "atx/vol/strategy.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;

void bind_dispersion(py::module_ &m) {
  // ── Universe ──
  py::class_<DispersionMember>(m, "DispersionMember")
      .def(py::init<>())
      .def_readwrite("symbol", &DispersionMember::symbol)
      .def_readwrite("uid", &DispersionMember::uid)
      .def_readwrite("weight", &DispersionMember::weight)
      .def("__repr__", [](const DispersionMember &d) {
        return "DispersionMember(symbol='" + d.symbol +
               "', weight=" + std::to_string(d.weight) + ")";
      });

  py::class_<DispersionUniverse>(m, "DispersionUniverse")
      .def(py::init<>())
      .def_readwrite("index", &DispersionUniverse::index)
      .def_readwrite("names", &DispersionUniverse::names)
      .def("__repr__", [](const DispersionUniverse &u) {
        return "DispersionUniverse(index='" + u.index.symbol +
               "', n_names=" + std::to_string(u.names.size()) + ")";
      });

  py::enum_<DispersionSide>(m, "DispersionSide")
      .value("SHORT_INDEX_LONG_NAMES", DispersionSide::ShortIndexLongNames)
      .value("LONG_INDEX_SHORT_NAMES", DispersionSide::LongIndexShortNames);

  // ── Run-directory readers ──
  py::class_<RunSpec>(m, "RunSpec")
      .def(py::init<>())
      .def_readwrite("label", &RunSpec::label)
      .def_readwrite("date_lo", &RunSpec::date_lo)
      .def_readwrite("date_hi", &RunSpec::date_hi)
      .def_readwrite("snapshot_suffix", &RunSpec::snapshot_suffix)
      .def_property(
          "opra_root", [](const RunSpec &s) { return s.opra_root.string(); },
          [](RunSpec &s, const std::string &v) { s.opra_root = v; })
      .def_readwrite("path_template", &RunSpec::path_template)
      .def_property(
          "universe_path", [](const RunSpec &s) { return s.universe_path.string(); },
          [](RunSpec &s, const std::string &v) { s.universe_path = v; })
      .def_property(
          "definitions_path", [](const RunSpec &s) { return s.definitions_path.string(); },
          [](RunSpec &s, const std::string &v) { s.definitions_path = v; })
      .def_property(
          "occ_ess_root", [](const RunSpec &s) { return s.occ_ess_root.string(); },
          [](RunSpec &s, const std::string &v) { s.occ_ess_root = v; })
      .def_readwrite("flat_rate", &RunSpec::flat_rate)
      .def_readwrite("min_names", &RunSpec::min_names)
      .def_readwrite("min_weight_coverage", &RunSpec::min_weight_coverage)
      .def_readwrite("target_dte_days", &RunSpec::target_dte_days)
      .def_readwrite("min_dte_days", &RunSpec::min_dte_days)
      .def_readwrite("max_dte_days", &RunSpec::max_dte_days)
      .def_readwrite("roll_dte_days", &RunSpec::roll_dte_days)
      .def_readwrite("gross_index_vega", &RunSpec::gross_index_vega)
      .def_readwrite("delta_band", &RunSpec::delta_band)
      .def_readwrite("fit_workers", &RunSpec::fit_workers)
      .def_readwrite("core_mode", &RunSpec::core_mode);

  py::class_<UniverseRow>(m, "UniverseRow")
      .def(py::init<>())
      .def_readwrite("effective_date", &UniverseRow::effective_date)
      .def_readwrite("symbol", &UniverseRow::symbol)
      .def_readwrite("raw_weight", &UniverseRow::raw_weight)
      .def_readwrite("source", &UniverseRow::source)
      .def_readwrite("as_of", &UniverseRow::as_of);

  m.def(
      "read_run_spec",
      [](const std::string &path) {
        return atxvol::python::unwrap(read_run_spec(std::filesystem::path{path}));
      },
      py::arg("path"), "Read a run directory's run_spec.tsv.");

  m.def(
      "read_universe",
      [](const std::string &path) {
        return atxvol::python::unwrap(read_universe(std::filesystem::path{path}));
      },
      py::arg("path"), "Read a point-in-time universe_schedule.tsv.");

  m.def(
      "universe_at",
      [](const std::vector<UniverseRow> &rows, const std::string &date) {
        return atxvol::python::unwrap(
            universe_at(std::span<const UniverseRow>{rows}, date));
      },
      py::arg("rows"), py::arg("date"),
      "Snap the universe as of `date`: SPY is the index leg, every other symbol "
      "whose effective_date <= date is a basket name.");

  m.def(
      "all_symbols",
      [](const std::vector<UniverseRow> &rows) {
        return all_symbols(std::span<const UniverseRow>{rows});
      },
      py::arg("rows"));

  // ── Projection route ──
  py::class_<DispersionBacktestConfig>(m, "DispersionBacktestConfig")
      .def(py::init<>())
      .def_readwrite("target_dte_days", &DispersionBacktestConfig::target_dte_days)
      .def_readwrite("roll_dte_days", &DispersionBacktestConfig::roll_dte_days)
      // REV-TAIL M-6. Bare `def_readwrite` with no docstring, on the one field
      // E1 changed the MEANING of. A Python caller tuned before E1 who keeps
      // their old number now builds a book 100x too large, and nothing on this
      // side of the boundary said so. The unit is stated where the caller meets
      // it, not only in the C++ header they never open.
      .def_readwrite("gross_index_vega", &DispersionBacktestConfig::gross_index_vega,
                     "Index-leg gross vega target, in DOLLARS OF VEGA PER VOL POINT "
                     "(a 0.01 move in sigma).\n\n"
                     "BREAKING CHANGE (E1): this field used to be read as dollars per "
                     "UNIT vol (per 1.00 of sigma), so the same value now builds a book "
                     "100x LARGER. Code tuned against the pre-E1 projected route must "
                     "DIVIDE its old value by 100. Must be > 0.")
      .def_readwrite("delta_band", &DispersionBacktestConfig::delta_band)
      .def_readwrite("min_names", &DispersionBacktestConfig::min_names)
      .def_readwrite("entry_every_n", &DispersionBacktestConfig::entry_every_n)
      .def_readwrite("project_to_calendar_expiry",
                     &DispersionBacktestConfig::project_to_calendar_expiry)
      .def_readwrite("record_diagnostics", &DispersionBacktestConfig::record_diagnostics)
      .def_readwrite("run", &DispersionBacktestConfig::run);

  py::class_<DispersionStrategy, IStrategy>(m, "DispersionStrategy");

  m.def(
      "make_dispersion_backtest_strategy",
      [](const DispersionUniverse &universe, const DispersionBacktestConfig &config) {
        return make_dispersion_backtest_strategy(universe, config);
      },
      py::arg("universe"), py::arg("config") = DispersionBacktestConfig{},
      "The canonical surface-only dispersion strategy (ATM-forward projected "
      "straddles, delta-hedged daily).");

  m.def(
      "run_dispersion_backtest",
      [](const Clock &clock, const DispersionUniverse &universe,
         const DispersionBacktestConfig &config) {
        py::gil_scoped_release release;
        return atxvol::python::unwrap(run_dispersion_backtest(clock, universe, config));
      },
      py::arg("clock"), py::arg("universe"), py::arg("config") = DispersionBacktestConfig{},
      "Projection route: run the canonical surface-only dispersion strategy "
      "over an already-qualified Clock.");

  // ── Listed route ──
  py::class_<ListedScheduleLeg>(m, "ListedScheduleLeg")
      .def(py::init<>())
      .def_readwrite("roll_date", &ListedScheduleLeg::roll_date)
      .def_readwrite("cohort", &ListedScheduleLeg::cohort)
      .def_readwrite("is_index", &ListedScheduleLeg::is_index)
      .def_readwrite("symbol", &ListedScheduleLeg::symbol)
      .def_readwrite("uid", &ListedScheduleLeg::uid)
      .def_readwrite("instrument_id", &ListedScheduleLeg::instrument_id)
      .def_readwrite("raw_symbol", &ListedScheduleLeg::raw_symbol)
      .def_readwrite("expiry_ts_ns", &ListedScheduleLeg::expiry_ts_ns)
      .def_readwrite("strike", &ListedScheduleLeg::strike)
      .def_readwrite("side", &ListedScheduleLeg::side)
      .def_readwrite("quantity", &ListedScheduleLeg::quantity)
      .def_readwrite("multiplier", &ListedScheduleLeg::multiplier)
      .def_readwrite("raw_bid", &ListedScheduleLeg::raw_bid)
      .def_readwrite("raw_ask", &ListedScheduleLeg::raw_ask)
      .def_readwrite("raw_mid", &ListedScheduleLeg::raw_mid)
      .def_readwrite("model_mark", &ListedScheduleLeg::model_mark)
      .def_readwrite("delta_per_share", &ListedScheduleLeg::delta_per_share)
      .def_readwrite("vega_per_unit_vol", &ListedScheduleLeg::vega_per_unit_vol)
      .def_readwrite("vega_per_contract_per_vol_point",
                     &ListedScheduleLeg::vega_per_contract_per_vol_point)
      .def_readwrite("normalized_weight", &ListedScheduleLeg::normalized_weight)
      .def_readwrite("achieved_leg_vega_per_vol_point",
                     &ListedScheduleLeg::achieved_leg_vega_per_vol_point)
      .def("__repr__", [](const ListedScheduleLeg &l) {
        return "ListedScheduleLeg(raw_symbol='" + l.raw_symbol + "', symbol='" + l.symbol +
               "', strike=" + std::to_string(l.strike) + ")";
      });

  py::class_<ListedScheduleRoll>(m, "ListedScheduleRoll")
      .def(py::init<>())
      .def_readwrite("roll_date", &ListedScheduleRoll::roll_date)
      .def_readwrite("valuation_ts_ns", &ListedScheduleRoll::valuation_ts_ns)
      .def_readwrite("cohort", &ListedScheduleRoll::cohort)
      .def_readwrite("expiry_ts_ns", &ListedScheduleRoll::expiry_ts_ns)
      .def_readwrite("gross_index_vega_target_per_vol_point",
                     &ListedScheduleRoll::gross_index_vega_target_per_vol_point)
      .def_readwrite("net_vega_per_vol_point", &ListedScheduleRoll::net_vega_per_vol_point)
      .def_readwrite("gross_vega_per_vol_point", &ListedScheduleRoll::gross_vega_per_vol_point)
      .def_readwrite("n_names", &ListedScheduleRoll::n_names)
      .def_readwrite("legs", &ListedScheduleRoll::legs)
      .def("__repr__", [](const ListedScheduleRoll &r) {
        return "ListedScheduleRoll(roll_date='" + r.roll_date +
               "', n_names=" + std::to_string(r.n_names) +
               ", n_legs=" + std::to_string(r.legs.size()) + ")";
      });

  py::class_<ListedDispersionSchedule>(m, "ListedDispersionSchedule")
      .def(py::init<>())
      .def_readwrite("rolls", &ListedDispersionSchedule::rolls)
      .def("__len__", [](const ListedDispersionSchedule &s) { return s.rolls.size(); })
      .def(
          "validate",
          [](const ListedDispersionSchedule &s, double max_relative_vega_residual) {
            atxvol::python::unwrap(
                validate_listed_dispersion_schedule(s, max_relative_vega_residual));
          },
          py::arg("max_relative_vega_residual") = 1.0e-10);

  m.def(
      "read_listed_dispersion_schedule",
      [](const std::string &path) {
        py::gil_scoped_release release;
        return atxvol::python::unwrap(read_listed_dispersion_schedule_file(path));
      },
      py::arg("path"), "Read a cold-authored trade_schedule.tsv of listed contracts.");

  m.def(
      "write_listed_dispersion_schedule",
      [](const std::string &path, const ListedDispersionSchedule &schedule) {
        py::gil_scoped_release release;
        atxvol::python::unwrap(write_listed_dispersion_schedule_file(path, schedule));
      },
      py::arg("path"), py::arg("schedule"));

  py::class_<ListedDispersionStrategy, IStrategy>(m, "ListedDispersionStrategy")
      .def_static(
          "create",
          [](ListedDispersionSchedule schedule, double delta_band) {
            return atxvol::python::unwrap(
                ListedDispersionStrategy::create(std::move(schedule), delta_band));
          },
          py::arg("schedule"), py::arg("delta_band") = 0.0,
          "Replay a frozen listed schedule. Requires QueryExecution::ColdReference "
          "and rejects a snapshot whose archive mark differs from the scheduled "
          "entry mark.")
      .def_property_readonly("schedule", &ListedDispersionStrategy::schedule)
      .def_property_readonly("all_rolls_consumed",
                             &ListedDispersionStrategy::all_rolls_consumed)
      .def_property_readonly("next_roll_index", &ListedDispersionStrategy::next_roll_index);
}
