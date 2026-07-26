#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/core/error.hpp"              // ErrorCode, to_string(ErrorCode)
#include "atx/vol/opra_hive.hpp"           // OpraHiveSpec (contractual defaults for r/snapshot)
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

// REV-R4 (review F-01). One failed cell, with the fitter's OWN reason attached.
//
// `detail` is the load-bearing field and the reason this dict exists: `code` is a
// coarse ErrorCode shared by every rejection class, so a dict carrying only
// (date, symbol, code) tells an operator that a cell died but not why — which is
// exactly the point at which they have to abandon the notebook and re-run the CLI
// to read its `failed_cell` lines. `code` is emitted as its NAME (the same
// spelling `atx-vol-surface-db-build` prints and the `--report` CSV's `code`
// column holds) so the three artifacts can be grepped with one string.
[[nodiscard]] py::dict failed_cell_to_dict(const FailedCell &c) {
  py::dict d;
  d["date"] = c.date;
  d["symbol"] = c.symbol;
  const std::string_view code_name = atx::core::to_string(c.code);
  d["code"] = std::string(code_name);
  d["detail"] = c.detail; // "" only when the fit Error carried no message
  return d;
}

// REV-R4. One stored surface a refused (or, under `allow_coverage_regression`,
// an allowed) rewrite would have destroyed. Same two-field shape the CLI prints.
[[nodiscard]] py::dict coverage_regression_cell_to_dict(const CoverageRegressionCell &c) {
  py::dict d;
  d["date"] = c.date;
  d["symbol"] = c.symbol; // CANONICAL archive key (ASCII-upper, truncated)
  return d;
}

// A plain nested dict mirroring SurfaceDbBuildReport, field for field.
//
// REV-R4 (review F-01) closed the one deliberate gap this comment used to
// describe: `coverage["failed_cells"]` was omitted, so the only Python-visible
// evidence of a lost cell was the `cells_failed` COUNT. The list is now carried
// in full, in the populate's own deterministic (date, symbol) order — NOT
// re-sorted here. The C++ side already guarantees that order for any
// `fit_workers` (surface_db_populate.hpp: appended by the single drain thread,
// dates ascending, symbols ascending within a date), and re-sorting in Python
// would both cost nothing and quietly invite a different comparator.
//
// No cap is applied either. The CLI's `--max-failures` bound is a PRESENTATION
// device for a terminal (`reported_failed_cells`); a dict is the programmatic
// artifact, the peer of the `--report` CSV, and that one is deliberately never
// truncated for the same reason: it is where an operator goes to root-cause.
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
  // REV-R4/REV-R3: the WRITE path's guard, distinct from the counter above it
  // (which is the PRE-fit filter's). `refused` = the guard held and every stored
  // surface is still on disk; `dropped` = `allow_coverage_regression` was passed
  // and they are gone. Both are always present, so a scripted diff of two runs
  // sees a regression appear.
  coverage["dates_refused_coverage_regression"] = r.coverage.dates_refused_coverage_regression;
  coverage["dates_dropped_coverage_regression"] = r.coverage.dates_dropped_coverage_regression;
  // REV-R4 (review F-01): the per-cell lists, complete and in the C++ side's own
  // deterministic (date, symbol) order — see report_to_dict's header comment for
  // why neither is capped and neither is re-sorted here.
  py::list failed_cells;
  for (const FailedCell &c : r.coverage.failed_cells) {
    failed_cells.append(failed_cell_to_dict(c));
  }
  coverage["failed_cells"] = std::move(failed_cells);
  py::list regression_cells;
  for (const CoverageRegressionCell &c : r.coverage.coverage_regression_cells) {
    regression_cells.append(coverage_regression_cell_to_dict(c));
  }
  coverage["coverage_regression_cells"] = std::move(regression_cells);
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
//
// REV-R4 (review F-01). Every keyword below `fit_workers` was ADDED by that fix,
// and `r` is the one that mattered: this binding used to build the spec without
// ever assigning `spec.hive.r`, so EVERY Python build ran at the default 0.0 with
// no way to say otherwise. That is not a missing convenience — a `--r` that does
// not match the rate the hive's quotes were priced under makes every
// put-call-parity forward wrong and fails every fit identically, and one
// production-shaped run at the wrong rate destroyed 95 stored surfaces during
// this feature's development. The production database was built at r = 0.043.
//
// The new keywords are APPENDED rather than inserted next to the hive arguments
// they belong with, so no existing positional call changes meaning.
//
// Defaults are read off the C++ structs' own defaults (`OpraHiveSpec{}`,
// `SurfaceDbBuildSpec{}`) rather than restated here, which is what keeps this
// binding and `atx-vol-surface-db-build` in agreement by construction instead of
// by anyone remembering to update both.
[[nodiscard]] py::dict
py_build_surface_db(const std::string &db_root, const std::string &hive_root,
                    const std::string &date_lo, const std::string &date_hi,
                    std::optional<std::vector<std::string>> symbols,
                    const std::string &index_symbol, const std::string &preset,
                    bool deep_selection, unsigned fit_workers, double r, bool retry_disabled,
                    bool pin_curve_family, const std::string &snapshot_suffix,
                    std::vector<double> yc_pillar_t, std::vector<double> yc_pillar_r,
                    bool allow_coverage_regression) {
  SurfaceDbBuildSpec spec;
  spec.db_root = db_root;
  spec.hive.root_dir = hive_root;
  spec.hive.date_lo = date_lo;
  spec.hive.date_hi = date_hi;
  if (symbols) {
    spec.hive.symbols = std::move(*symbols); // else empty => discover every underlying
  }
  // REV-R4 F-01. The whole point of the fix: the rate REACHES the fitter.
  spec.hive.r = r;
  spec.hive.snapshot_suffix = snapshot_suffix;
  // Two or more strictly-ascending pillars build a YieldCurve; empty or one
  // pillar leaves the flat `r` in force. A LENGTH MISMATCH between the two is a
  // malformed spec and `load_opra_hive` returns InvalidArgument, which `unwrap`
  // raises as AtxError — deliberately not pre-validated here, so the Python and
  // C++ callers get the identical verdict from the identical check.
  spec.hive.yc_pillar_t = std::move(yc_pillar_t);
  spec.hive.yc_pillar_r = std::move(yc_pillar_r);
  const FitPreset fp = parse_preset(preset);
  spec.preset = fp;             // populate fallback tier
  spec.auto_config.preset = fp; // manifest seed tier
  spec.auto_config.index_symbol = index_symbol;
  spec.auto_config.deep_selection = deep_selection;
  spec.auto_config.retry_disabled = retry_disabled;
  spec.auto_config.pin_curve_family = pin_curve_family;
  spec.fit_workers = fit_workers;
  spec.allow_coverage_regression = allow_coverage_regression;

  SurfaceDbBuildReport report;
  {
    py::gil_scoped_release release;
    report = atxvol::python::unwrap(build_surface_db(spec));
  }
  return report_to_dict(report);
}

// The build driver's docstring. Kept beside the function because most of it is
// about `r`, and `r` is a knob whose WRONG value silently deletes data.
constexpr const char *kBuildSurfaceDbDoc = R"doc(
Build (or resume) a production SurfaceDb from an OPRA hive v2 tree.

Chains hive load -> per-symbol config generation -> cell-aware streaming populate
in one call, exactly as ``atx-vol-surface-db-build`` does; every keyword below
mirrors that CLI's flag of the same name and carries the same default. Releases
the GIL for the whole C++ call.

Parameters
----------
db_root, hive_root : str
    SurfaceDb root (created if absent, else opened and RESUMED) and the OPRA
    hive v2 root holding ``date=<YYYY-MM-DD>/data.parquet``.
date_lo, date_hi : str
    Inclusive ``YYYY-MM-DD`` window. Every CALENDAR date in range is enumerated,
    so weekends and holidays show up as missing dates and that is healthy.
symbols : list[str] | None
    Explicit universe, or None/empty to DISCOVER every underlying in the window.
index_symbol : str
    Designated index leg, pinned to the dense index recipe. Under the default
    ``pin_curve_family=False`` this is a config-time annotation only.
preset : str
    ``populate`` (default) | ``fast`` | ``accurate`` | ``robust`` | ``hft``.
deep_selection : bool
    Additionally run the full held-out ``select_curve`` OOS search per symbol.
fit_workers : int
    Outer fit fan-out; 0 = auto (honors ``ATX_VOL_FIT_WORKERS``), 1 = serial.
    A PERF-ONLY knob: results are byte-identical for any value.
r : float
    Flat continuously-compounded carry rate, default ``0.0`` -- the same default
    ``atx-vol-surface-db-build --r`` has, so the two agree.

    ``0.0`` MEANS "NO CARRY" AND IS VERY LIKELY WRONG FOR REAL QUOTES. This value
    must match the rate the hive's quotes were priced under. If it does not,
    every put-call-parity forward is wrong and every full fit fails identically;
    on a database that already holds surfaces, a whole-file partition rewrite at
    the wrong rate can DESTROY the stored surfaces that fail to re-fit (see
    ``allow_coverage_regression``). One production-shaped run at a wrong rate
    removed 95 stored surfaces before the guard existed. The default is left at
    the CLI's ``0.0`` rather than at some safer-looking number precisely so the
    two tools cannot disagree -- it is not a recommendation.
retry_disabled : bool
    Re-attempt symbols whose STORED config is disabled instead of skipping them.
    Default False: without it a fail-closed disable is permanent for the life of
    the database. It DOES re-enable a symbol an operator disabled by hand.
pin_curve_family : bool
    Store the auto-selected curve family as a HARD PIN (default False = record it
    as the preferred route only). Pinning gives each cell exactly one family
    attempt and disables both of PricerFitter's recovery ladders.
snapshot_suffix : str
    Per-date snapshot stamp appended to the date, default ``"T19:55:00Z"`` (the
    pre-close minute). Passed verbatim as ``OpraLoadSpec.snapshot_iso``.
yc_pillar_t, yc_pillar_r : list[float]
    Optional term-structure pillars applied to every cell absent a per-cell
    market-input override. Two or more strictly-ascending year-fractions build a
    YieldCurve; empty or a single pillar leaves the flat ``r`` in force. The two
    lists must be the SAME LENGTH -- a mismatch raises ``AtxError``.
allow_coverage_regression : bool
    Permit a date's rewrite to DESTROY a stored surface. Default False (guard ON):
    such a date is REFUSED, its existing partition left untouched, and the CLI
    equivalent exits 5. Pass True only for a run that INTENDS retirement; the
    destroyed cells are named in ``coverage["coverage_regression_cells"]``.

    IT WAIVES THE REFUSAL AND NOTHING ELSE. A date whose existing partition FILE
    is present and will not OPEN still aborts the whole build with ``AtxError``,
    with or without this flag: a caller who authorised destroying a NAMED list has
    not answered a question about contents nobody could read. The remedy for a
    genuinely corrupt partition is to delete the file and re-run.

Returns
-------
dict
    A plain nested dict mirroring ``SurfaceDbBuildReport``: ``config``,
    ``coverage`` and the four ingest counters. ``coverage["failed_cells"]`` is
    the COMPLETE per-cell failure list -- one dict of
    ``{date, symbol, code, detail}`` each, where ``detail`` is the fitter's own
    message -- in the C++ side's deterministic (date, symbol) order, never
    truncated and never re-sorted.

    THIS CALL DOES NOT RAISE ON A COVERAGE REGRESSION, AND YOU MUST CHECK FOR ONE
    YOURSELF. The CLI turns a refusal into exit 5; this binding has no exit code
    and deliberately does not invent an exception for it, so a run in which the
    guard refused EVERY date returns successfully and looks like any other result
    dict. ``coverage["cells_ok"]`` counts FITS, not commits, so it can be large on
    a run that wrote nothing at all. The three keys that carry the verdict:

    - ``coverage["dates_refused_coverage_regression"]`` -- dates NOT written
      because the rewrite would have destroyed a stored surface. Non-zero means
      the run did not do what you asked; the existing partitions are intact and
      nothing was lost. This is the key to branch on for the CLI's exit-5
      behaviour.
    - ``coverage["dates_dropped_coverage_regression"]`` -- dates written ANYWAY
      under ``allow_coverage_regression=True``. Non-zero means the surfaces named
      below are GONE.
    - ``coverage["coverage_regression_cells"]`` -- the COMPLETE, uncapped
      ``{date, symbol}`` list behind those two counters, ascending. On the
      destructive path this list is the ONLY record those surfaces ever existed:
      the archive format keeps no tombstone, so a destroyed cell is byte-for-byte
      a cell that was never fitted. Persist it.

    ``coverage["dates_written"]`` counts dates that really COMMITTED, so it is the
    other half of the same check.
)doc";

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

  // REV-R4 (review F-01): defaults are taken from the C++ specs' own defaults so
  // this binding and the CLI cannot drift apart. `r` in particular is
  // `OpraHiveSpec{}.r` == 0.0 == `--r`'s default; see kBuildSurfaceDbDoc for why
  // that default is deliberately NOT something safer-looking.
  const OpraHiveSpec hive_defaults{};
  const SurfaceDbBuildSpec build_defaults{};
  m.def("build_surface_db", &py_build_surface_db, kBuildSurfaceDbDoc, py::arg("db_root"),
        py::arg("hive_root"), py::arg("date_lo"), py::arg("date_hi"),
        py::arg("symbols") = py::none(), py::arg("index_symbol") = std::string{},
        py::arg("preset") = std::string{"populate"}, py::arg("deep_selection") = false,
        py::arg("fit_workers") = 0U, py::arg("r") = hive_defaults.r,
        py::arg("retry_disabled") = build_defaults.auto_config.retry_disabled,
        py::arg("pin_curve_family") = build_defaults.auto_config.pin_curve_family,
        py::arg("snapshot_suffix") = hive_defaults.snapshot_suffix,
        py::arg("yc_pillar_t") = hive_defaults.yc_pillar_t,
        py::arg("yc_pillar_r") = hive_defaults.yc_pillar_r,
        py::arg("allow_coverage_regression") = build_defaults.allow_coverage_regression);
}
