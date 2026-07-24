// Bindings for priced-surface construction and the on-disk SurfaceDb.
//
// This is the write side of the stack: build a `CurveSurface` slice by slice,
// seal it into a `PricedSurface`, and archive a set of them under one partition
// key. With these in place a corpus can be authored entirely from Python rather
// than only consumed from one a C++ tool produced.
//
// Lifetime note: `SurfaceArchiveItem` is doubly non-owning (a `string_view`
// symbol and a raw `const PricedSurface*`). `write_partition` below therefore
// materializes the symbol strings into a vector that is reserved up front — so
// no reallocation invalidates a view — and borrows the surfaces from live
// Python objects held by the caller's list for the duration of the call.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/american.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_db.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

void write_partition(SurfaceDb &db, const std::string &key, const py::list &items,
                     const ArchiveV2WriteOpts &opts) {
  const std::size_t n = items.size();
  if (n == 0) {
    throw py::value_error("items must not be empty");
  }
  // Reserve first: `SurfaceArchiveItem::symbol` is a string_view into `symbols`,
  // so a later reallocation would dangle it.
  std::vector<std::string> symbols;
  std::vector<const PricedSurface *> surfaces;
  symbols.reserve(n);
  surfaces.reserve(n);
  for (const auto handle : items) {
    auto entry = py::cast<py::sequence>(handle);
    if (entry.size() != 2) {
      throw py::value_error("each item must be a (symbol, PricedSurface) pair");
    }
    symbols.push_back(py::cast<std::string>(entry[0]));
    surfaces.push_back(py::cast<const PricedSurface *>(entry[1]));
  }

  std::vector<SurfaceArchiveItem> archive_items;
  archive_items.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    archive_items.push_back(SurfaceArchiveItem{symbols[i], surfaces[i], std::nullopt});
  }
  {
    py::gil_scoped_release release;
    atxvol::python::unwrap(db.write_partition(key, archive_items, opts));
  }
}

} // namespace

void bind_surface_db(py::module_ &m) {
  // ── Slice context + pricing context ──
  py::class_<SliceContext>(m, "SliceContext")
      .def(py::init<>())
      .def(py::init([](double T, double forward, double borrow, double q_eff, std::size_t n_used,
                       std::size_t n_dropped) {
             return SliceContext{T, forward, borrow, q_eff, n_used, n_dropped};
           }),
           py::arg("T"), py::arg("forward"), py::arg("borrow") = 0.0, py::arg("q_eff") = 0.0,
           py::arg("n_used") = 0, py::arg("n_dropped") = 0)
      .def_readwrite("T", &SliceContext::T)
      .def_readwrite("forward", &SliceContext::forward)
      .def_readwrite("borrow", &SliceContext::borrow)
      .def_readwrite("q_eff", &SliceContext::q_eff)
      .def_readwrite("n_used", &SliceContext::n_used)
      .def_readwrite("n_dropped", &SliceContext::n_dropped);

  py::class_<PricingContext>(m, "PricingContext")
      .def(py::init<>())
      .def_readwrite("S", &PricingContext::S)
      .def_readwrite("r", &PricingContext::r)
      .def_readwrite("now_ts_ns", &PricingContext::now_ts_ns)
      .def_readwrite("method", &PricingContext::method)
      .def_readwrite("al_opts", &PricingContext::al_opts)
      .def_readwrite("uid", &PricingContext::uid);

  py::enum_<VolCurveKind>(m, "VolCurveKind")
      .value("CONVEX_DENSE", VolCurveKind::ConvexDense)
      .value("ESSVI", VolCurveKind::Essvi)
      .value("SVI", VolCurveKind::Svi)
      .value("LINEAR_VARIANCE", VolCurveKind::LinearVariance)
      .value("C8", VolCurveKind::C8)
      .value("SPLINE_VOL", VolCurveKind::SplineVol);

  // ── CurveSurface. Slices are pushed by parameter set rather than by handing
  //    over a `unique_ptr<IVolCurve>`, which keeps ownership entirely C++-side. ──
  py::class_<CurveSurface>(m, "CurveSurface")
      .def(py::init<>())
      .def(
          "push_essvi",
          [](CurveSurface &self, const EssviParams &slice, double df) {
            self.push(std::make_unique<EssviCurve>(slice, df));
          },
          py::arg("slice"), py::arg("df"),
          "Append an eSSVI slice. Slices must be pushed in non-decreasing T.")
      .def(
          "push_svi",
          [](CurveSurface &self, const SviParams &slice, double df) {
            self.push(std::make_unique<SviCurve>(slice, df));
          },
          py::arg("slice"), py::arg("df"))
      .def("clone", &CurveSurface::clone)
      .def_property_readonly("n_slices", &CurveSurface::n_slices)
      .def_property_readonly("empty", &CurveSurface::empty)
      .def("__len__", &CurveSurface::n_slices)
      // `w` / `iv` each have a private bracket-resolved overload, so a plain
      // member pointer is ambiguous — pin the public arity with a lambda.
      .def(
          "w", [](const CurveSurface &self, double k_log, double t) { return self.w(k_log, t); },
          py::arg("k_log"), py::arg("T"))
      .def(
          "iv", [](const CurveSurface &self, double k_log, double t) { return self.iv(k_log, t); },
          py::arg("k_log"), py::arg("T"))
      .def("forward_at", &CurveSurface::forward_at, py::arg("T"));

  // ── PricedSurface (move-only; `create` consumes the CurveSurface). ──
  py::class_<PricedSurface>(m, "PricedSurface")
      .def_static(
          "create",
          [](CurveSurface &surface, std::vector<SliceContext> context,
             const PricingContext &pricing) {
            if (surface.empty()) {
              throw py::value_error(
                  "curve surface is empty (or was already consumed by a previous create call)");
            }
            return atxvol::python::unwrap(
                PricedSurface::create(std::move(surface), std::move(context), pricing));
          },
          py::arg("surface"), py::arg("context"), py::arg("pricing"),
          "Seal a CurveSurface into a priced surface. NOTE: `surface` is MOVED "
          "FROM and is left empty; do not reuse it.")
      .def("iv", &PricedSurface::iv, py::arg("K"), py::arg("T"))
      .def("total_variance", &PricedSurface::total_variance, py::arg("K"), py::arg("T"))
      .def(
          "fair_value",
          [](const PricedSurface &self, double k, double t, Side side, QueryExecution execution) {
            return atxvol::python::unwrap(self.fair_value(k, t, side, execution));
          },
          py::arg("K"), py::arg("T"), py::arg("side"),
          py::arg("execution") = QueryExecution::Configured)
      .def(
          "greeks",
          [](const PricedSurface &self, double k, double t, Side side, QueryExecution execution) {
            return atxvol::python::unwrap(self.greeks(k, t, side, execution));
          },
          py::arg("K"), py::arg("T"), py::arg("side"),
          py::arg("execution") = QueryExecution::Configured)
      .def(
          "delta",
          [](const PricedSurface &self, double k, double t, Side side, QueryExecution execution) {
            return atxvol::python::unwrap(self.delta(k, t, side, execution));
          },
          py::arg("K"), py::arg("T"), py::arg("side"),
          py::arg("execution") = QueryExecution::Configured)
      .def(
          "vega",
          [](const PricedSurface &self, double k, double t, Side side, QueryExecution execution) {
            return atxvol::python::unwrap(self.vega(k, t, side, execution));
          },
          py::arg("K"), py::arg("T"), py::arg("side"),
          py::arg("execution") = QueryExecution::Configured)
      .def("forward_at", &PricedSurface::forward_at, py::arg("T"))
      .def("q_eff_at", &PricedSurface::q_eff_at, py::arg("T"))
      .def("rate_at", &PricedSurface::rate_at, py::arg("T"))
      .def_property_readonly("n_slices", &PricedSurface::n_slices)
      .def_property_readonly("uid", &PricedSurface::uid)
      .def_property_readonly("instance_id", &PricedSurface::instance_id)
      .def_property_readonly("query_pricing_tier", &PricedSurface::query_pricing_tier)
      .def_property_readonly("pricing", &PricedSurface::pricing,
                             py::return_value_policy::reference_internal)
      .def_property_readonly("context",
                             [](const PricedSurface &self) {
                               const auto ctx = self.context();
                               return std::vector<SliceContext>{ctx.begin(), ctx.end()};
                             })
      .def("kind_at", &PricedSurface::kind_at, py::arg("index"));

  // ── Archive / db options ──
  py::class_<ArchiveV2WriteOpts>(m, "ArchiveV2WriteOpts")
      .def(py::init<>())
      .def_readwrite("flags", &ArchiveV2WriteOpts::flags)
      .def_readwrite("lookup_load_pct", &ArchiveV2WriteOpts::lookup_load_pct)
      .def_readwrite("surface_alignment", &ArchiveV2WriteOpts::surface_alignment)
      .def_readwrite("created_ts_ns", &ArchiveV2WriteOpts::created_ts_ns);

  py::class_<SurfaceDbCreateOpts>(m, "SurfaceDbCreateOpts")
      .def(py::init<>())
      .def_readwrite("created_ts_ns", &SurfaceDbCreateOpts::created_ts_ns)
      .def_readwrite("partition_cache_capacity", &SurfaceDbCreateOpts::partition_cache_capacity);

  py::class_<SurfaceDbOpenOpts>(m, "SurfaceDbOpenOpts")
      .def(py::init<>())
      .def_readwrite("partition_cache_capacity", &SurfaceDbOpenOpts::partition_cache_capacity);

  py::class_<SurfaceDbCacheStats>(m, "SurfaceDbCacheStats")
      .def_readonly("resident", &SurfaceDbCacheStats::resident)
      .def_readonly("capacity", &SurfaceDbCacheStats::capacity);

  py::class_<DbPartitionInfo>(m, "DbPartitionInfo")
      .def_readonly("key", &DbPartitionInfo::key)
      .def_readonly("surface_count", &DbPartitionInfo::surface_count)
      .def_readonly("file_size", &DbPartitionInfo::file_size)
      .def_readonly("created_ts_ns", &DbPartitionInfo::created_ts_ns)
      .def("__repr__", [](const DbPartitionInfo &p) {
        return "DbPartitionInfo(key='" + p.key +
               "', surface_count=" + std::to_string(p.surface_count) + ")";
      });

  // ── SurfaceDb (move-only: constructed only through the static factories) ──
  py::class_<SurfaceDb>(m, "SurfaceDb")
      .def_static(
          "create",
          [](const std::string &root, const SurfaceDbCreateOpts &opts) {
            return atxvol::python::unwrap(SurfaceDb::create(root, opts));
          },
          py::arg("root"), py::arg("opts") = SurfaceDbCreateOpts{})
      .def_static(
          "open",
          [](const std::string &root, const SurfaceDbOpenOpts &opts) {
            return atxvol::python::unwrap(SurfaceDb::open(root, opts));
          },
          py::arg("root"), py::arg("opts") = SurfaceDbOpenOpts{})
      .def_property_readonly("root", &SurfaceDb::root)
      .def_property_readonly("generation", &SurfaceDb::generation)
      .def("symbols", &SurfaceDb::symbols)
      .def("partitions", &SurfaceDb::partitions)
      .def("partition_cache_stats", &SurfaceDb::partition_cache_stats)
      .def("write_partition", &write_partition, py::arg("key"), py::arg("items"),
           py::arg("opts") = ArchiveV2WriteOpts{},
           "Archive [(symbol, PricedSurface), ...] under one partition key.")
      .def(
          "load_surface",
          [](const SurfaceDb &self, const std::string &key, const std::string &symbol) {
            return atxvol::python::unwrap(self.load_surface(key, symbol));
          },
          py::arg("key"), py::arg("symbol"))
      .def(
          "drop_partition",
          [](SurfaceDb &self, const std::string &key) {
            atxvol::python::unwrap(self.drop_partition(key));
          },
          py::arg("key"))
      .def("refresh",
           [](SurfaceDb &self) { atxvol::python::unwrap(self.refresh()); });
}
