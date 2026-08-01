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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/opra_hive.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/priced_surface_view.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_db.hpp"
#include "atx/vol/surface_db_build.hpp"
#include "atx/vol/surface_db_populate.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"
#include "batch_status.hpp"
#include "result.hpp"
#include "sides.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

using DoubleArray = py::array_t<double, py::array::c_style | py::array::forcecast>;

std::span<const double> as_double_span(const DoubleArray &array, const char *name) {
  if (array.ndim() != 1) {
    throw py::value_error(std::string{name} + " must be a one-dimensional array");
  }
  return {array.data(), static_cast<std::size_t>(array.size())};
}

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

// ── Y3: vectorized PricedSurface query ──────────────────────────────────────
//
// The query methods were scalar-per-call and held the GIL, so a chain-scale
// valuation from Python was a for-loop paying pybind dispatch on every point and
// blocking every other thread throughout. `grid` walks the whole (K, T, side)
// selection in one C++ pass under one GIL release, writing preallocated numpy
// buffers, on the Y1(c) NaN + per-lane status convention: a point the surface
// cannot serve NaNs its own row and records its ErrorCode, it does not abort the
// grid.
py::dict priced_surface_grid(const PricedSurface &self, const DoubleArray &k_array,
                             const DoubleArray &t_array, const py::object &raw_side,
                             QueryExecution execution) {
  // FIX-5 (final-review Minor): dtype kind validated before the cast (sides.hpp).
  const atxvol::python::SideCodes side_array = atxvol::python::as_side_codes(raw_side);
  const auto k = as_double_span(k_array, "K");
  const auto t = as_double_span(t_array, "T");
  if (t.size() != k.size()) {
    throw py::value_error("T must have the same length as K");
  }
  if (side_array.ndim() != 1 || static_cast<std::size_t>(side_array.size()) != k.size()) {
    throw py::value_error("side must be a one-dimensional array as long as K");
  }
  const auto n = static_cast<py::ssize_t>(k.size());
  py::array_t<double> iv(n), w(n), value(n), delta(n), gamma(n), vega(n), theta(n), rho(n),
      vanna(n), volga(n), charm(n);
  // F-5: every independently evaluated output family gets its own status and
  // validity channel. `iv` / `total_variance` expose only a bare-double C++
  // API, so a non-finite value maps to OutOfRange; fair value and Greeks retain
  // their exact ErrorCode. The legacy row `status` remains as the first error
  // in column order, but it is no longer the only diagnostic.
  py::array_t<std::int32_t> status(n), iv_status(n), w_status(n), value_status(n),
      greeks_status(n);
  py::array_t<bool> iv_valid(n), w_valid(n), value_valid(n), greeks_valid(n),
      delta_valid(n), gamma_valid(n), vega_valid(n), theta_valid(n), rho_valid(n),
      vanna_valid(n), volga_valid(n), charm_valid(n);
  // Decoded up front, with the GIL still held and before a single point is
  // priced: one shared decoder (I2, `sides.hpp`), and an unrecognised code is a
  // rejected CALL rather than a half-written grid.
  std::vector<Side> sides(k.size());
  {
    const auto *codes = side_array.data();
    for (std::size_t i = 0; i < k.size(); ++i) {
      sides[i] = atxvol::python::decode_side(codes[i], i);
    }
  }
  // Every `mutable_data()` hoisted above the release (M3): these reach into a
  // Python object's internals, and `american_slice` in pricing.cpp already sets
  // that precedent.
  auto *iv_p = iv.mutable_data();
  auto *w_p = w.mutable_data();
  auto *value_p = value.mutable_data();
  auto *delta_p = delta.mutable_data();
  auto *gamma_p = gamma.mutable_data();
  auto *vega_p = vega.mutable_data();
  auto *theta_p = theta.mutable_data();
  auto *rho_p = rho.mutable_data();
  auto *vanna_p = vanna.mutable_data();
  auto *volga_p = volga.mutable_data();
  auto *charm_p = charm.mutable_data();
  auto *status_p = status.mutable_data();
  auto *iv_status_p = iv_status.mutable_data();
  auto *w_status_p = w_status.mutable_data();
  auto *value_status_p = value_status.mutable_data();
  auto *greeks_status_p = greeks_status.mutable_data();
  auto *iv_valid_p = iv_valid.mutable_data();
  auto *w_valid_p = w_valid.mutable_data();
  auto *value_valid_p = value_valid.mutable_data();
  auto *greeks_valid_p = greeks_valid.mutable_data();
  auto *delta_valid_p = delta_valid.mutable_data();
  auto *gamma_valid_p = gamma_valid.mutable_data();
  auto *vega_valid_p = vega_valid.mutable_data();
  auto *theta_valid_p = theta_valid.mutable_data();
  auto *rho_valid_p = rho_valid.mutable_data();
  auto *vanna_valid_p = vanna_valid.mutable_data();
  auto *volga_valid_p = volga_valid.mutable_data();
  auto *charm_valid_p = charm_valid.mutable_data();
  {
    py::gil_scoped_release release;
    constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
    constexpr auto kOutOfRange =
        static_cast<std::int32_t>(atx::core::ErrorCode::OutOfRange);
    for (std::size_t i = 0; i < k.size(); ++i) {
      const Side side = sides[i];
      iv_p[i] = self.iv(k[i], t[i]);
      w_p[i] = self.total_variance(k[i], t[i]);
      iv_valid_p[i] = std::isfinite(iv_p[i]);
      w_valid_p[i] = std::isfinite(w_p[i]);
      iv_status_p[i] =
          iv_valid_p[i] ? atxvol::python::kStatusOk : kOutOfRange;
      w_status_p[i] =
          w_valid_p[i] ? atxvol::python::kStatusOk : kOutOfRange;
      status_p[i] = iv_status_p[i] != atxvol::python::kStatusOk
                        ? iv_status_p[i]
                        : w_status_p[i];
      auto fv = self.fair_value(k[i], t[i], side, execution);
      if (!fv) {
        value_p[i] = kNan;
        value_valid_p[i] = false;
        value_status_p[i] = static_cast<std::int32_t>(fv.error().code());
        if (status_p[i] == atxvol::python::kStatusOk) {
          status_p[i] = value_status_p[i];
        }
      } else {
        value_p[i] = *fv;
        value_valid_p[i] = std::isfinite(value_p[i]);
        value_status_p[i] =
            value_valid_p[i] ? atxvol::python::kStatusOk : kOutOfRange;
        if (status_p[i] == atxvol::python::kStatusOk &&
            value_status_p[i] != atxvol::python::kStatusOk) {
          status_p[i] = value_status_p[i];
        }
      }
      auto g = self.greeks(k[i], t[i], side, execution);
      if (!g) {
        delta_p[i] = gamma_p[i] = vega_p[i] = theta_p[i] = kNan;
        rho_p[i] = vanna_p[i] = volga_p[i] = charm_p[i] = kNan;
        greeks_valid_p[i] = false;
        delta_valid_p[i] = gamma_valid_p[i] = vega_valid_p[i] =
            theta_valid_p[i] = false;
        rho_valid_p[i] = vanna_valid_p[i] = volga_valid_p[i] =
            charm_valid_p[i] = false;
        greeks_status_p[i] = static_cast<std::int32_t>(g.error().code());
        if (status_p[i] == atxvol::python::kStatusOk) {
          status_p[i] = greeks_status_p[i];
        }
        continue;
      }
      delta_p[i] = g->delta;
      gamma_p[i] = g->gamma;
      vega_p[i] = g->vega;
      theta_p[i] = g->theta;
      rho_p[i] = g->rho;
      vanna_p[i] = g->vanna;
      volga_p[i] = g->volga;
      charm_p[i] = g->charm;
      delta_valid_p[i] = std::isfinite(delta_p[i]);
      gamma_valid_p[i] = std::isfinite(gamma_p[i]);
      vega_valid_p[i] = std::isfinite(vega_p[i]);
      theta_valid_p[i] = std::isfinite(theta_p[i]);
      rho_valid_p[i] = std::isfinite(rho_p[i]);
      vanna_valid_p[i] = std::isfinite(vanna_p[i]);
      volga_valid_p[i] = std::isfinite(volga_p[i]);
      charm_valid_p[i] = std::isfinite(charm_p[i]);
      greeks_valid_p[i] =
          delta_valid_p[i] && gamma_valid_p[i] && vega_valid_p[i] &&
          theta_valid_p[i] && rho_valid_p[i] && vanna_valid_p[i] &&
          volga_valid_p[i] && charm_valid_p[i];
      greeks_status_p[i] =
          greeks_valid_p[i] ? atxvol::python::kStatusOk : kOutOfRange;
      if (status_p[i] == atxvol::python::kStatusOk &&
          greeks_status_p[i] != atxvol::python::kStatusOk) {
        status_p[i] = greeks_status_p[i];
      }
    }
  }
  py::dict out;
  out["iv"] = std::move(iv);
  out["total_variance"] = std::move(w);
  out["fair_value"] = std::move(value);
  out["delta"] = std::move(delta);
  out["gamma"] = std::move(gamma);
  out["vega"] = std::move(vega);
  out["theta"] = std::move(theta);
  out["rho"] = std::move(rho);
  out["vanna"] = std::move(vanna);
  out["volga"] = std::move(volga);
  out["charm"] = std::move(charm);
  out["status"] = std::move(status);
  out["iv_status"] = std::move(iv_status);
  out["total_variance_status"] = std::move(w_status);
  out["fair_value_status"] = std::move(value_status);
  out["greeks_status"] = std::move(greeks_status);
  out["iv_valid"] = std::move(iv_valid);
  out["total_variance_valid"] = std::move(w_valid);
  out["fair_value_valid"] = std::move(value_valid);
  out["greeks_valid"] = std::move(greeks_valid);
  out["delta_valid"] = std::move(delta_valid);
  out["gamma_valid"] = std::move(gamma_valid);
  out["vega_valid"] = std::move(vega_valid);
  out["theta_valid"] = std::move(theta_valid);
  out["rho_valid"] = std::move(rho_valid);
  out["vanna_valid"] = std::move(vanna_valid);
  out["volga_valid"] = std::move(volga_valid);
  out["charm_valid"] = std::move(charm_valid);
  return out;
}

[[nodiscard]] std::string partition_key_from_python(const py::object &value) {
  if (py::isinstance<py::str>(value)) {
    return py::cast<std::string>(value);
  }
  if (py::isinstance<DbPartitionInfo>(value)) {
    return py::cast<const DbPartitionInfo &>(value).key;
  }
  throw py::type_error("partition key must be a string or DbPartitionInfo");
}

} // namespace

void bind_surface_core(py::module_ &m) {
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
      .def("grid", &priced_surface_grid, py::arg("K"), py::arg("T"), py::arg("side"),
           py::arg("execution") = QueryExecution::Configured,
           "Vectorized query over a (K, T, side) selection.\n\n"
           "Returns a dict of numpy columns — iv, total_variance, fair_value and\n"
           "every Greek — plus lossless per-family status/validity arrays:\n"
           "`iv_status`/`iv_valid`, `total_variance_status`/\n"
           "`total_variance_valid`, `fair_value_status`/`fair_value_valid`, and\n"
           "`greeks_status`/`greeks_valid`; every individual Greek also has a\n"
           "`<name>_valid` mask. Only a shape mismatch or an\n"
           "unrecognised `side` code raises. Releases the GIL for the whole walk.\n\n"
           "`status` is retained for compatibility as the first failure in\n"
           "(iv, total_variance, fair_value, greeks) order. New code should use\n"
           "the family-specific channel, since families fail independently.\n"
           "The bare-double iv/variance API maps a non-finite output to\n"
           "ErrorCode.OUT_OF_RANGE; Result-returning families retain their exact\n"
           "ErrorCode.")
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
      .def_readonly("config_fingerprint", &DbPartitionInfo::config_fingerprint)
      // Main's original Python surface returned bare partition-key strings,
      // while pipeline-m returns the richer metadata records. String-like
      // comparison/order keeps old list/sorted code working without discarding
      // the metadata fields needed by the newer API.
      .def("__str__", [](const DbPartitionInfo &p) { return p.key; })
      .def("__hash__", [](const DbPartitionInfo &p) {
        return py::hash(py::str(p.key));
      })
      .def("__eq__",
           [](const DbPartitionInfo &a, const DbPartitionInfo &b) {
             return a.key == b.key;
           },
           py::is_operator())
      .def("__eq__",
           [](const DbPartitionInfo &a, const std::string &b) {
             return a.key == b;
           },
           py::is_operator())
      .def("__lt__",
           [](const DbPartitionInfo &a, const DbPartitionInfo &b) {
             return a.key < b.key;
           },
           py::is_operator())
      .def("__lt__",
           [](const DbPartitionInfo &a, const std::string &b) {
             return a.key < b;
           },
           py::is_operator())
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
      .def(
          "partition_keys",
          [](const SurfaceDb &self) {
            std::vector<std::string> keys;
            for (const DbPartitionInfo &partition : self.partitions()) {
              keys.push_back(partition.key);
            }
            return keys;
          },
          "Partition keys only, preserving main's original lightweight view.")
      .def("partition_cache_stats", &SurfaceDb::partition_cache_stats)
      .def("write_partition", &write_partition, py::arg("key"), py::arg("items"),
           py::arg("opts") = ArchiveV2WriteOpts{},
           "Archive [(symbol, PricedSurface), ...] under one partition key.")
      .def(
          "session_ts",
          [](const SurfaceDb &self, const py::object &key) {
            const std::string normalized_key = partition_key_from_python(key);
            py::gil_scoped_release release;
            return atxvol::python::unwrap(self.session_ts(normalized_key));
          },
          py::arg("key"),
          "Read a partition's market timestamp from its first record header.")
      .def(
          "load_surface",
          [](const SurfaceDb &self, const py::object &key, const std::string &symbol) {
            const std::string normalized_key = partition_key_from_python(key);
            py::gil_scoped_release release;
            return atxvol::python::unwrap(self.load_surface(normalized_key, symbol));
          },
          py::arg("key"), py::arg("symbol"))
      .def(
          "map_surface",
          [](const SurfaceDb &self, const py::object &key, const std::string &symbol) {
            const std::string normalized_key = partition_key_from_python(key);
            py::gil_scoped_release release;
            return atxvol::python::unwrap(self.map_surface(normalized_key, symbol));
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

namespace {

// Map the snake_case preset name (the Python kwarg) to a FitPreset. The default
// is "populate" (the bulk-populate tier); the other names cover the fit-quality
// ladder in session.hpp.
[[nodiscard]] FitPreset parse_preset(const std::string &name) {
  if (name == "populate") {
    return FitPreset::Populate;
  }
  if (name == "bulk") {
    return FitPreset::Bulk; // Perf 2b opt-in throughput tier (session.hpp)
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
  // REV-R3 fix-2 (review N-3): a SUBSET of the key above — the refusals whose
  // partition file is on disk but unlisted in the manifest. The CLI reads it to
  // pick which cause its banner names; a Python caller has no banner at all, so
  // this is the only way it can tell the two causes apart.
  coverage["dates_refused_partition_unlisted"] = r.coverage.dates_refused_partition_unlisted;
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
  //
  // REV-R5 (review M-3): and it is CHECKED FIRST, the same way `--r` is. The CLI
  // parses this value with a dedicated `parse_finite_double` under which `--r nan`,
  // `--r inf` and `--r 0.03x` are hard usage errors (exit 2), with a paragraph
  // explaining why: every `--r` value is a claim about the market, so a silently
  // wrong one is the trap the flag exists to close. Python's float literal cannot
  // produce `0.03x`, but `float("nan")` and `math.inf` are one keystroke away and
  // `load_opra_hive` validates root, dates and pillar lengths only — never
  // finiteness. Without this, a notebook could start a full production build at
  // `r = nan`: every put-call-parity forward is NaN, every fit fails identically,
  // and on a database that already holds surfaces the whole-file rewrite is the
  // shape that destroyed 95 of them.
  //
  // `py::value_error` is the binding's established analogue of the CLI's exit 2 —
  // the same choice `parse_preset` makes for an unknown preset name — so the two
  // front ends reject the same inputs with equivalent verdicts. It is raised BEFORE
  // the GIL is released and before any db is opened or created, so a rejected call
  // touches nothing.
  if (!std::isfinite(r)) {
    throw py::value_error("build_surface_db: r must be finite (got nan or inf). The carry rate "
                          "is a claim about the market that every put-call-parity forward "
                          "depends on; a non-finite one fails every fit identically and, on a "
                          "database that already holds surfaces, the whole-file rewrite that "
                          "follows can destroy them. This matches "
                          "`atx-vol-surface-db-build --r`, which rejects nan/inf as a usage "
                          "error.");
  }
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

    MUST BE FINITE. ``nan`` and ``inf`` raise ``ValueError`` before anything is
    opened or created, matching ``atx-vol-surface-db-build --r``, which rejects
    them as a usage error (exit 2) rather than letting a silently-wrong rate reach
    the fitter. ``load_opra_hive`` does not check this itself -- it validates the
    root, the dates and the pillar lengths only -- so the guard lives here.
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
    a run that wrote nothing at all. The four keys that carry the verdict:

    - ``coverage["dates_refused_coverage_regression"]`` -- dates NOT written
      because the rewrite would have destroyed a stored surface. Non-zero means
      the run did not do what you asked; the existing partitions are intact and
      nothing was lost. This is the key to branch on for the CLI's exit-5
      behaviour.
    - ``coverage["dates_refused_partition_unlisted"]`` -- a SUBSET of the count
      above (never larger), telling you WHY those dates were refused, which
      decides what to do about it. Zero means the cells this run offered really
      did fail to fit and the carry rate is the first thing to check. Non-zero
      means that many of the refused dates have a partition file on disk that the
      manifest does not list -- nothing failed to fit; the index and the disk
      disagree, and the fix is to re-run those dates over the FULL board set (or
      delete the stale file), NOT to pass ``allow_coverage_regression=True``,
      which would delete the surfaces that survived.
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

    NOR DOES IT RAISE WHEN THE BUILD PRODUCED NOTHING AT ALL, AND THAT IS THE
    OTHER CHECK YOU MUST WRITE YOURSELF. The paragraph above covers the coverage
    regression only; the CLI has FOUR further verdicts, all of which it reports as
    exit 3 and all of which are equally invisible here -- a caller who follows the
    paragraph above exactly still misses every one of them. This binding returns a
    normal dict for all four. Their conditions, in the CLI's own order (the exact
    predicates, from surface_db_build.hpp):

    - TOTAL LOAD FAILURE -- ``n_dates_loaded == 0 and n_load_errors > 0``. Every
      present file in the window was unreadable, so not one board reached the
      fitter. NOT the same as an empty window: ``n_load_errors == 0`` with nothing
      present is a healthy no-op (weekends, an un-pulled range) and must stay one.
    - TOTAL CONFIG FAILURE -- no symbol has an enabled config and nothing was
      produced: ``config["n_disabled_failed"] + config["n_disabled_existing"] > 0``,
      ``config["n_configured"] + (config["n_skipped_existing"] -
      config["n_disabled_existing"]) == 0``, ``coverage["cells_ok"] == 0`` and
      ``coverage["cells_carried"] == 0``. The database will stay empty.
    - TOTAL FIT FAILURE -- ``coverage["cells_to_fit"] > 0`` and
      ``coverage["cells_ok"] == 0`` and ``coverage["cells_carried"] == 0``. Work
      was scheduled, nothing fitted, nothing was carried. ``r`` is the top suspect.
    - THE ``--strict`` READING -- ``coverage["cells_to_fit"] > 0`` and
      ``coverage["cells_ok"] == 0``, carry IGNORED. The CLI applies this one only
      under ``--strict``, which has no keyword here on purpose: it is a reporting
      choice, not a build input. If you are an unattended scheduler over a database
      whose failing-cell set is expected to be EMPTY, this is your check. If your
      database holds permanently-failing cells -- surfaces that will never fit --
      it is TRUE ON EVERY RUN of a perfectly healthy database, which is why the CLI
      does not make it the default either.

    NO RAISE IS ADDED FOR ANY OF THESE, DELIBERATELY. Whether this call should
    raise on a dead build is an API decision about every existing caller's failure
    semantics, not a documentation fix; changing it silently would break notebooks
    that already branch on the dict. What is fixed here is that the docstring no
    longer warns about one invisible verdict while staying silent about four more.
)doc";

} // namespace

void bind_surface_db(py::module_ &m) {
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
      .def("rate_at", [](const LoadedSurface &s, double T) { return s.view.rate_at(T); },
           py::arg("T"))
      .def_property_readonly("uid", [](const LoadedSurface &s) { return s.view.uid(); })
      .def_property_readonly("n_slices", [](const LoadedSurface &s) { return s.view.n_slices(); });

  // Register the richer authoring/query API after LoadedSurface, since
  // SurfaceDb.map_surface returns that type.
  bind_surface_core(m);

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
