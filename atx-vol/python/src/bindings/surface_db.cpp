#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/priced_surface.hpp"      // PricedSurface
#include "atx/vol/priced_surface_view.hpp" // PricedSurfaceView (LoadedSurface::view)
#include "atx/vol/session.hpp"             // FitPreset
#include "atx/vol/surface_db.hpp"          // SurfaceDb, LoadedSurface, DbPartitionInfo
#include "atx/vol/surface_db_build.hpp"    // SurfaceDbBuildSpec/Report, build_surface_db
#include "atx/vol/surface_db_populate.hpp" // UniversePopulateCoverage, PopulateSymbolStats
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

// Map the snake_case preset name (the Python kwarg) to a FitPreset. The default
// is "populate" (the bulk-populate tier); the other names cover the fit-quality
// ladder in session.hpp.
[[nodiscard]] FitPreset parse_preset(const std::string &name) {
  if (name == "populate") {
    return FitPreset::Populate;
  }
  if (name == "fast") {
    return FitPreset::Fast;
  }
  if (name == "accurate") {
    return FitPreset::Accurate;
  }
  if (name == "robust") {
    return FitPreset::Robust;
  }
  if (name == "hft") {
    return FitPreset::Hft;
  }
  throw py::value_error("unknown preset '" + name +
                        "' (expected one of: populate, fast, accurate, robust, hft)");
}

[[nodiscard]] py::dict symbol_stats_to_dict(const PopulateSymbolStats &s) {
  py::dict d;
  d["symbol"] = s.symbol;
  d["n_attempted"] = s.n_attempted;
  d["n_ok"] = s.n_ok;
  d["n_failed"] = s.n_failed;
  d["n_disabled"] = s.n_disabled;
  // FIX-D fix-1 (I2): the third disposition of the same cells. Without it a
  // carried symbol reads attempted=1, ok=0, failed=0, disabled=0 in python too.
  d["n_carried"] = s.n_carried;
  d["mean_oos_in_band"] = s.mean_oos_in_band;
  return d;
}

// A plain nested dict mirroring SurfaceDbBuildReport, with one deliberate gap:
// `coverage` does NOT carry `failed_cells` (the per-cell fit-failure-reason
// list added alongside this comment's original "field-for-field" claim,
// which that addition made false). Every other config/coverage field below
// is a direct field-for-field copy.
[[nodiscard]] py::dict report_to_dict(const SurfaceDbBuildReport &r) {
  py::dict config;
  config["n_symbols"] = r.config.n_symbols;
  config["n_configured"] = r.config.n_configured;
  config["n_skipped_existing"] = r.config.n_skipped_existing;
  config["n_disabled_failed"] = r.config.n_disabled_failed;
  config["n_disabled_existing"] = r.config.n_disabled_existing;
  // The STANDING disabled set (this run's failures + the ones already stored
  // disabled and skipped), so a resumed build names its casualties too.
  config["failed_symbols"] = r.config.failed_symbols;

  py::dict coverage;
  coverage["cells_loaded"] = r.coverage.cells_loaded;
  coverage["cells_to_fit"] = r.coverage.cells_to_fit;
  coverage["cells_refit"] = r.coverage.cells_refit;
  // FIX-D fix-1 (I2): the only signal that carry-over engaged. The converged
  // carry resume reports cells_ok = 0 and cells_refit = 0, which is otherwise
  // indistinguishable from a build that did nothing.
  coverage["cells_carried"] = r.coverage.cells_carried;
  // FIX-E: stored cells of a DISABLED symbol, preserved through a rewrite rather
  // than deleted. Kept out of `cells_carried` because that counter is read as
  // evidence the run produced a serviceable database.
  coverage["cells_carried_disabled"] = r.coverage.cells_carried_disabled;
  coverage["cells_already_present"] = r.coverage.cells_already_present;
  coverage["cells_ok"] = r.coverage.cells_ok;
  coverage["cells_failed"] = r.coverage.cells_failed;
  coverage["dates_total"] = r.coverage.dates_total;
  coverage["dates_written"] = r.coverage.dates_written;
  coverage["dates_skipped_complete"] = r.coverage.dates_skipped_complete;
  coverage["dates_skipped_would_drop"] = r.coverage.dates_skipped_would_drop;
  py::list per_symbol;
  for (const auto &s : r.coverage.per_symbol) {
    per_symbol.append(symbol_stats_to_dict(s));
  }
  coverage["per_symbol"] = std::move(per_symbol);

  py::dict out;
  out["config"] = std::move(config);
  out["coverage"] = std::move(coverage);
  out["n_dates_loaded"] = r.n_dates_loaded;
  out["n_dates_missing"] = r.n_dates_missing;
  out["n_load_errors"] = r.n_load_errors;
  out["n_coverage_holes"] = r.n_coverage_holes;
  return out;
}

// One-call production build driver (surface_db_build.hpp). `symbols=None` selects
// discover-all (an empty hive symbol list). Releases the GIL around the C++ call.
[[nodiscard]] py::dict py_build_surface_db(const std::string &db_root, const std::string &hive_root,
                                          const std::string &date_lo, const std::string &date_hi,
                                          std::optional<std::vector<std::string>> symbols,
                                          const std::string &index_symbol,
                                          const std::string &preset, bool deep_selection,
                                          unsigned fit_workers) {
  SurfaceDbBuildSpec spec;
  spec.db_root = db_root;
  spec.hive.root_dir = hive_root;
  spec.hive.date_lo = date_lo;
  spec.hive.date_hi = date_hi;
  if (symbols) {
    spec.hive.symbols = std::move(*symbols); // else empty => discover every underlying
  }
  const FitPreset fp = parse_preset(preset);
  spec.preset = fp;             // populate fallback tier
  spec.auto_config.preset = fp; // manifest seed tier
  spec.auto_config.index_symbol = index_symbol;
  spec.auto_config.deep_selection = deep_selection;
  spec.fit_workers = fit_workers;

  SurfaceDbBuildReport report;
  {
    py::gil_scoped_release release;
    report = atxvol::python::unwrap(build_surface_db(spec));
  }
  return report_to_dict(report);
}

} // namespace

void bind_surface_db(py::module_ &m) {
  // Owned surface handle (load_surface). Exposes the tier-independent query
  // vocabulary; enough to prove a built partition round-trips.
  py::class_<PricedSurface>(m, "PricedSurface")
      .def("iv", &PricedSurface::iv, py::arg("K"), py::arg("T"))
      .def("total_variance", &PricedSurface::total_variance, py::arg("K"), py::arg("T"));

  // Zero-copy surface handle (map_surface): co-owns the partition mapping and
  // forwards queries to the borrowed view.
  py::class_<LoadedSurface>(m, "LoadedSurface")
      .def(
          "iv", [](const LoadedSurface &s, double K, double T) { return s.view.iv(K, T); },
          py::arg("K"), py::arg("T"))
      .def(
          "total_variance",
          [](const LoadedSurface &s, double K, double T) { return s.view.total_variance(K, T); },
          py::arg("K"), py::arg("T"))
      .def_property_readonly("uid", [](const LoadedSurface &s) { return s.view.uid(); })
      .def_property_readonly("n_slices", [](const LoadedSurface &s) { return s.view.n_slices(); });

  py::class_<SurfaceDb>(m, "SurfaceDb")
      .def_static(
          "open",
          [](const std::string &root) { return atxvol::python::unwrap(SurfaceDb::open(root)); },
          py::arg("root"))
      .def_property_readonly("root", &SurfaceDb::root)
      .def("symbols", &SurfaceDb::symbols)
      .def("partitions",
           [](const SurfaceDb &db) {
             std::vector<std::string> keys;
             for (const auto &p : db.partitions()) {
               keys.push_back(p.key);
             }
             return keys;
           })
      .def(
          "load_surface",
          [](const SurfaceDb &db, const std::string &key, const std::string &symbol) {
            return atxvol::python::unwrap(db.load_surface(key, symbol));
          },
          py::arg("key"), py::arg("symbol"), py::call_guard<py::gil_scoped_release>())
      .def(
          "map_surface",
          [](const SurfaceDb &db, const std::string &key, const std::string &symbol) {
            return atxvol::python::unwrap(db.map_surface(key, symbol));
          },
          py::arg("key"), py::arg("symbol"), py::call_guard<py::gil_scoped_release>());

  m.def("build_surface_db", &py_build_surface_db, py::arg("db_root"), py::arg("hive_root"),
        py::arg("date_lo"), py::arg("date_hi"), py::arg("symbols") = py::none(),
        py::arg("index_symbol") = std::string{}, py::arg("preset") = std::string{"populate"},
        py::arg("deep_selection") = false, py::arg("fit_workers") = 0U);
}
