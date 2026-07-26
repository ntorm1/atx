#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/american.hpp"
#include "atx/vol/american_batch.hpp"
#include "atx/vol/american_iv.hpp"
#include "atx/vol/batch.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/types.hpp"
#include "batch_status.hpp"
#include "result.hpp"
#include "sides.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

using DoubleArray = py::array_t<double, py::array::c_style | py::array::forcecast>;

std::span<const double> as_span(const DoubleArray &array, const char *name) {
  if (array.ndim() != 1) {
    throw py::value_error(std::string{name} + " must be a one-dimensional array");
  }
  return {array.data(), static_cast<std::size_t>(array.size())};
}

void require_same_size(std::size_t expected, std::span<const double> value, const char *name) {
  if (value.size() != expected) {
    throw py::value_error(std::string{name} + " must have the same length as F");
  }
}

py::array_t<double> price_batch(const DoubleArray &f_array, const DoubleArray &k_array,
                                const DoubleArray &t_array, const DoubleArray &sigma_array,
                                const DoubleArray &df_array, Side side) {
  const auto f = as_span(f_array, "F");
  const auto k = as_span(k_array, "K");
  const auto t = as_span(t_array, "T");
  const auto sigma = as_span(sigma_array, "sigma");
  const auto df = as_span(df_array, "df");
  require_same_size(f.size(), k, "K");
  require_same_size(f.size(), t, "T");
  require_same_size(f.size(), sigma, "sigma");
  require_same_size(f.size(), df, "df");

  py::array_t<double> output(static_cast<py::ssize_t>(f.size()));
  std::vector<Side> sides(f.size(), side);
  // Hoisted above the release (M3): `mutable_data()` reaches into a Python
  // object's internals, and `american_slice` below already sets that precedent.
  const std::span<double> out{output.mutable_data(), static_cast<std::size_t>(output.size())};
  {
    py::gil_scoped_release release;
    atxvol::python::unwrap(black76_price_batch(f, k, t, sigma, df, sides, out));
  }
  return output;
}

// PY-3: NaN + per-lane status, the convention `batch.hpp` already documents and
// the binding used to erase. `implied_vol_batch` writes NaN into the value slot
// and the lane's Error into the parallel status span; raising on the first bad
// lane discarded every good one, which is fatal on real NBBO chains (crossed
// markets, prices below intrinsic). Only a malformed CALL raises now.
std::pair<py::array_t<double>, py::array_t<std::int32_t>>
iv_batch(const DoubleArray &price_array, const DoubleArray &f_array, const DoubleArray &k_array,
         const DoubleArray &t_array, const DoubleArray &df_array, Side side) {
  const auto price = as_span(price_array, "price");
  const auto f = as_span(f_array, "F");
  const auto k = as_span(k_array, "K");
  const auto t = as_span(t_array, "T");
  const auto df = as_span(df_array, "df");
  require_same_size(f.size(), price, "price");
  require_same_size(f.size(), k, "K");
  require_same_size(f.size(), t, "T");
  require_same_size(f.size(), df, "df");

  py::array_t<double> output(static_cast<py::ssize_t>(f.size()));
  std::vector<Side> sides(f.size(), side);
  std::vector<Status> statuses(f.size());
  const std::span<double> out{output.mutable_data(), static_cast<std::size_t>(output.size())};
  {
    py::gil_scoped_release release;
    atxvol::python::unwrap(implied_vol_batch(price, f, k, t, df, sides, out, statuses));
  }
  return {std::move(output), atxvol::python::to_status_array(statuses)};
}

py::array_t<double> american_slice(const DoubleArray &strikes_array, double spot, double t,
                                   double sigma, double r, double q, Side side,
                                   const std::optional<AlOpts> &opts) {
  const auto strikes = as_span(strikes_array, "strikes");
  py::array_t<double> output(static_cast<py::ssize_t>(strikes.size()));
  auto out = std::span<double>{output.mutable_data(), static_cast<std::size_t>(output.size())};
  {
    py::gil_scoped_release release;
    Status status = side == Side::Call
                        ? andersen_lake_call_slice(spot, strikes, t, sigma, r, q, out, opts)
                        : andersen_lake_put_slice(spot, strikes, t, sigma, r, q, out, opts);
    atxvol::python::unwrap(std::move(status));
  }
  return output;
}

// ── Y3: numpy-native American batch ─────────────────────────────────────────
//
// `american_batch.hpp` is the SoA laned flagship and was entirely unbound, so
// chain-scale American valuation from Python was a for-loop paying ~1-2 us of
// pybind dispatch per contract against a sub-microsecond kernel. These entry
// points hand the whole book to the C++ batch in ONE call under ONE GIL release,
// on the same NaN + per-lane status convention Y1(c) established
// (`batch_status.hpp`).


// FIX-5 (final-review Minor): the array arrives UNTYPED so the caller's dtype is
// still visible; `as_side_codes` rejects a float kind before any cast (see
// sides.hpp) and returns the int32 view. int64 columns keep working.
std::vector<Side> as_sides(const py::object &raw, std::size_t expected) {
  const atxvol::python::SideCodes array = atxvol::python::as_side_codes(raw);
  if (array.ndim() != 1) {
    throw py::value_error("side must be a one-dimensional array");
  }
  if (static_cast<std::size_t>(array.size()) != expected) {
    throw py::value_error("side must have the same length as S");
  }
  std::vector<Side> out(expected);
  const auto *data = array.data();
  for (std::size_t i = 0; i < expected; ++i) {
    // One shared decoder (I2): an unrecognised code is rejected, never folded
    // onto Call. See sides.hpp.
    out[i] = atxvol::python::decode_side(data[i], i);
  }
  return out;
}

// LaneStatus is a two-state Ok/Unsupported BATCH-REGIME flag, not an
// `atx::core::Status`. F-5: do not collapse Unsupported into
// ErrorCode::NotImplemented — that made it indistinguishable from a scalar
// pricer genuinely returning NotImplemented. The negative sentinel is outside
// ErrorCode's non-negative domain and is published to Python below.
inline constexpr std::int32_t kAmericanBatchUnsupportedRegime = -1;

py::array_t<std::int32_t> lane_status_array(std::span<const LaneStatus> lanes) {
  py::array_t<std::int32_t> out(static_cast<py::ssize_t>(lanes.size()));
  auto *data = out.mutable_data();
  for (std::size_t i = 0; i < lanes.size(); ++i) {
    data[i] = lanes[i] == LaneStatus::Ok
                  ? atxvol::python::kStatusOk
                  : kAmericanBatchUnsupportedRegime;
  }
  return out;
}

struct BookSpans {
  std::span<const double> s, k, t, sigma, r, q;
  std::vector<Side> side;
  std::size_t n{0};
};

BookSpans as_book(const DoubleArray &s_array, const DoubleArray &k_array,
                  const DoubleArray &t_array, const DoubleArray &sigma_array,
                  const DoubleArray &r_array, const DoubleArray &q_array,
                  const py::object &side_array) {
  BookSpans book;
  book.s = as_span(s_array, "S");
  book.n = book.s.size();
  book.k = as_span(k_array, "K");
  book.t = as_span(t_array, "T");
  book.sigma = as_span(sigma_array, "sigma");
  book.r = as_span(r_array, "r");
  book.q = as_span(q_array, "q");
  require_same_size(book.n, book.k, "K");
  require_same_size(book.n, book.t, "T");
  require_same_size(book.n, book.sigma, "sigma");
  require_same_size(book.n, book.r, "r");
  require_same_size(book.n, book.q, "q");
  book.side = as_sides(side_array, book.n);
  return book;
}

// C2 (rev-ws-y): `method` and `opts` are HONOURED, never accepted and dropped.
//
// The laned flagship `american_price_batch(in, out, kernel, ws)` has no channel
// for either — only `american_price_batch_resolved` carries them, and that
// entry point requires a single broadcast (S, T, r, q), which a general book
// does not have. So the binding routes on engagement instead of lying about it:
//
//   * default engagement (Andersen-Lake, no AlOpts) -> the vectorized batch,
//     which is exactly what that route computes;
//   * ANY other engagement -> the exact scalar `american_price` per lane, under
//     the same single GIL release. That is the same patch-to-scalar rule the
//     batch already applies to its own non-pack lanes, so the answer is
//     bit-identical to a loop over `american_price` for every value the
//     signature admits (`test_batch.py` pins all four).
//
// Silently returning the Andersen-Lake price for `method=BAW` with STATUS_OK on
// every lane — 2.8e-2, ~0.45% on a $6-8 option — is the exact "silent wrong
// numbers with no diagnostic channel" class this sprint exists to kill.
std::pair<py::array_t<double>, py::array_t<std::int32_t>>
american_price_batch_py(const DoubleArray &s_array, const DoubleArray &k_array,
                        const DoubleArray &t_array, const DoubleArray &sigma_array,
                        const DoubleArray &r_array, const DoubleArray &q_array,
                        const py::object &side_array, AmericanMethod method,
                        const std::optional<AlOpts> &opts, simd::SimdIsa isa) {
  const BookSpans book = as_book(s_array, k_array, t_array, sigma_array, r_array, q_array,
                                 side_array);
  py::array_t<double> prices(static_cast<py::ssize_t>(book.n));
  // Hoisted ABOVE every GIL release: `mutable_data()` touches a Python object's
  // internals, and `american_slice` already sets that precedent in this file.
  auto *price_p = prices.mutable_data();

  if (method != AmericanMethod::AndersenLake || opts.has_value()) {
    py::array_t<std::int32_t> status(static_cast<py::ssize_t>(book.n));
    auto *status_p = status.mutable_data();
    {
      py::gil_scoped_release release;
      constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
      for (std::size_t i = 0; i < book.n; ++i) {
        auto priced = american_price(book.s[i], book.k[i], book.t[i], book.sigma[i], book.r[i],
                                     book.q[i], book.side[i], method, opts);
        if (!priced) {
          price_p[i] = kNan;
          status_p[i] = static_cast<std::int32_t>(priced.error().code());
          continue;
        }
        price_p[i] = *priced;
        status_p[i] = atxvol::python::kStatusOk;
      }
    }
    return {std::move(prices), std::move(status)};
  }

  PriceBatchOutput out;
  PricingWorkspace ws;
  PricingKernel kernel;
  kernel.isa = isa;
  {
    py::gil_scoped_release release;
    AmericanBatchInput in;
    in.S = book.s;
    in.K = book.k;
    in.T = book.t;
    in.sigma = book.sigma;
    in.r = book.r;
    in.q = book.q;
    in.side = book.side;
    out.resize(book.n);
    ws.reserve_lanes(book.n);
    atxvol::python::unwrap(american_price_batch(in, out, kernel, ws));
    if (book.n > 0) {
      std::copy(out.price.begin(), out.price.end(), price_p);
    }
  }
  return {std::move(prices), lane_status_array(out.status)};
}

py::dict american_greeks_batch_py(const DoubleArray &s_array, const DoubleArray &k_array,
                                  const DoubleArray &t_array, const DoubleArray &sigma_array,
                                  const DoubleArray &r_array, const DoubleArray &q_array,
                                  const py::object &side_array, bool analytic,
                                  simd::SimdIsa isa) {
  const BookSpans book = as_book(s_array, k_array, t_array, sigma_array, r_array, q_array,
                                 side_array);
  const auto n = static_cast<py::ssize_t>(book.n);
  py::array_t<double> delta(n), gamma(n), vega(n), theta(n), rho(n), vanna(n), volga(n),
      charm(n), price(n);
  PricingWorkspace ws;
  PricingKernel kernel;
  kernel.isa = isa;
  kernel.analytic_greeks = analytic;
  // Every `mutable_data()` hoisted above the release (M3).
  simd::GreeksBatchSoA soa;
  soa.delta = delta.mutable_data();
  soa.gamma = gamma.mutable_data();
  soa.vega = vega.mutable_data();
  soa.theta = theta.mutable_data();
  soa.rho = rho.mutable_data();
  soa.vanna = vanna.mutable_data();
  soa.volga = volga.mutable_data();
  soa.charm = charm.mutable_data();
  soa.price = price.mutable_data();
  {
    py::gil_scoped_release release;
    AmericanBatchInput in;
    in.S = book.s;
    in.K = book.k;
    in.T = book.t;
    in.sigma = book.sigma;
    in.r = book.r;
    in.q = book.q;
    in.side = book.side;
    ws.reserve_lanes(book.n);
    atxvol::python::unwrap(american_greeks_batch(in, GreekFieldMask::All, soa, kernel, ws));
  }
  py::dict out;
  out["delta"] = std::move(delta);
  out["gamma"] = std::move(gamma);
  out["vega"] = std::move(vega);
  out["theta"] = std::move(theta);
  out["rho"] = std::move(rho);
  out["vanna"] = std::move(vanna);
  out["volga"] = std::move(volga);
  out["charm"] = std::move(charm);
  out["price"] = std::move(price);
  out["status"] = lane_status_array(ws.lane_status_view());
  return out;
}

std::pair<py::array_t<double>, py::array_t<std::int32_t>>
american_iv_batch_py(const DoubleArray &price_array, double spot, const DoubleArray &k_array,
                     double t, double r, double q, Side side, AmericanMethod method, double tol,
                     std::uint16_t max_iter, const std::optional<AlOpts> &opts,
                     bool warm_start_chain) {
  const auto price = as_span(price_array, "price");
  const auto k = as_span(k_array, "K");
  if (k.size() != price.size()) {
    throw py::value_error("K must have the same length as price");
  }
  py::array_t<double> ivs(static_cast<py::ssize_t>(k.size()));
  std::vector<Status> statuses(k.size());
  const std::span<double> out{ivs.mutable_data(), static_cast<std::size_t>(ivs.size())};
  {
    py::gil_scoped_release release;
    atxvol::python::unwrap(american_implied_vol_batch(price, spot, k, t, r, q, side, out,
                                                      statuses, method, tol, max_iter, opts,
                                                      nullptr, warm_start_chain));
  }
  return {std::move(ivs), atxvol::python::to_status_array(statuses)};
}

} // namespace

void bind_pricing(py::module_ &m) {
  py::enum_<atx::core::ErrorCode>(m, "ErrorCode")
      .value("UNKNOWN", atx::core::ErrorCode::Unknown)
      .value("INVALID_ARGUMENT", atx::core::ErrorCode::InvalidArgument)
      .value("OUT_OF_RANGE", atx::core::ErrorCode::OutOfRange)
      .value("NOT_FOUND", atx::core::ErrorCode::NotFound)
      .value("ALREADY_EXISTS", atx::core::ErrorCode::AlreadyExists)
      .value("PERMISSION_DENIED", atx::core::ErrorCode::PermissionDenied)
      .value("UNAVAILABLE", atx::core::ErrorCode::Unavailable)
      .value("INTERNAL", atx::core::ErrorCode::Internal)
      .value("NOT_IMPLEMENTED", atx::core::ErrorCode::NotImplemented)
      .value("IO_ERROR", atx::core::ErrorCode::IoError)
      .value("PARSE_ERROR", atx::core::ErrorCode::ParseError);

  // Sentinel for the NaN + per-lane status convention (batch_status.hpp): every
  // vectorized entry point returns a parallel int32 status array whose entries
  // are STATUS_OK or int(ErrorCode). ErrorCode::Unknown is 0, so success needs a
  // value from outside the enum.
  m.attr("STATUS_OK") = atxvol::python::kStatusOk;
  // The optimized American lane has a distinct two-state regime contract, not
  // an ErrorCode. This negative sentinel cannot collide with the error enum and
  // means "unsupported by this batch regime; inspect/route the lane separately".
  m.attr("AMERICAN_BATCH_UNSUPPORTED_REGIME") =
      kAmericanBatchUnsupportedRegime;

  py::enum_<Side>(m, "Side").value("CALL", Side::Call).value("PUT", Side::Put).export_values();
  py::enum_<ExerciseStyle>(m, "ExerciseStyle")
      .value("EUROPEAN", ExerciseStyle::European)
      .value("AMERICAN", ExerciseStyle::American);
  py::enum_<PricingRoute>(m, "PricingRoute")
      .value("B76_ONLY", PricingRoute::B76Only)
      .value("B76_AL_CACHE", PricingRoute::B76AlCache)
      .value("B76_AL_COLD", PricingRoute::B76AlCold)
      .value("PRIOR_SURFACE", PricingRoute::PriorSurface);
  py::enum_<AmericanMethod>(m, "AmericanMethod")
      .value("ANDERSEN_LAKE", AmericanMethod::AndersenLake)
      .value("BAW", AmericanMethod::Baw);

  py::class_<Black76Aux>(m, "Black76Aux")
      .def_readonly("price", &Black76Aux::price)
      .def_readonly("d1", &Black76Aux::d1)
      .def_readonly("d2", &Black76Aux::d2)
      .def_readonly("n_d1", &Black76Aux::n_d1);
  py::class_<Black76ValueVega>(m, "Black76ValueVega")
      .def_readonly("price", &Black76ValueVega::price)
      .def_readonly("vega", &Black76ValueVega::vega);
  py::class_<Greeks>(m, "Greeks")
      .def_readonly("delta", &Greeks::delta)
      .def_readonly("gamma", &Greeks::gamma)
      .def_readonly("vega", &Greeks::vega)
      .def_readonly("theta", &Greeks::theta)
      .def_readonly("rho", &Greeks::rho)
      .def_readonly("vanna", &Greeks::vanna)
      .def_readonly("volga", &Greeks::volga)
      .def_readonly("charm", &Greeks::charm);
  py::class_<Black76Greeks>(m, "Black76Greeks")
      .def_readonly("greeks", &Black76Greeks::greeks)
      .def_readonly("price", &Black76Greeks::price);

  py::class_<AlOpts>(m, "AlOpts")
      .def(py::init<>())
      .def(py::init<std::uint16_t, std::uint16_t, std::uint16_t, double>(),
           py::arg("n_collocation"), py::arg("n_quadrature"), py::arg("max_newton_iter"),
           py::arg("tol"))
      .def_readwrite("n_collocation", &AlOpts::n_collocation)
      .def_readwrite("n_quadrature", &AlOpts::n_quadrature)
      .def_readwrite("max_newton_iter", &AlOpts::max_newton_iter)
      .def_readwrite("tol", &AlOpts::tol)
      .def_static("default", &al_default_opts)
      .def_static("fast", &al_fast_opts);

  py::class_<AmericanGreeks>(m, "AmericanGreeks")
      .def_readonly("delta", &AmericanGreeks::delta)
      .def_readonly("gamma", &AmericanGreeks::gamma)
      .def_readonly("vega", &AmericanGreeks::vega)
      .def_readonly("theta", &AmericanGreeks::theta)
      .def_readonly("rho", &AmericanGreeks::rho)
      .def_readonly("vanna", &AmericanGreeks::vanna)
      .def_readonly("volga", &AmericanGreeks::volga)
      .def_readonly("charm", &AmericanGreeks::charm)
      .def_readonly("price", &AmericanGreeks::price);

  // G2: carry sensitivities (∂P/∂q; ∂P/∂Div via the chain rule at the C++ layer).
  py::class_<CarryGreeks>(m, "CarryGreeks")
      .def_readonly("price", &CarryGreeks::price)
      .def_readonly("dP_dq", &CarryGreeks::dP_dq)
      .def_readonly("q_one_sided", &CarryGreeks::q_one_sided);

  // G4: early-assignment risk screen (heuristic — carry benefit vs remaining time value).
  py::class_<AssignmentRisk>(m, "AssignmentRisk")
      .def_readonly("at_risk", &AssignmentRisk::at_risk)
      .def_readonly("margin", &AssignmentRisk::margin)
      .def_readonly("carry_benefit", &AssignmentRisk::carry_benefit)
      .def_readonly("time_value", &AssignmentRisk::time_value);

  py::class_<AloPricer>(m, "AloPricer")
      .def(py::init<double, double, double, double, double, Side, const std::optional<AlOpts> &>(),
           py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("r"), py::arg("q"),
           py::arg("side"), py::arg("opts") = std::nullopt)
      // PY-5: NO `gil_scoped_release` here. `AloPricer::price` is non-const — it
      // mutates the cached exercise boundary — so releasing the GIL let two
      // Python threads sharing one pricer race in C++ with no lock: undefined
      // behaviour, not a slow path. Holding the GIL is the whole synchronization
      // story for this object. The long-running kernels that legitimately release
      // it (`andersen_lake`, the batch entry points, `run_backtest`) are all
      // stateless or own their scratch.
      .def("price", &AloPricer::price, py::arg("sigma"),
           "Price at `sigma`, reusing this pricer's cached exercise boundary.\n\n"
           "Mutates cached state, so this call holds the GIL: one `AloPricer` is\n"
           "safe to share across Python threads, and concurrency comes from using\n"
           "one pricer per thread or from the batch entry points.");

  m.def("black76_price", &black76_price, py::arg("F"), py::arg("K"), py::arg("T"), py::arg("sigma"),
        py::arg("df"), py::arg("side"), py::call_guard<py::gil_scoped_release>());
  m.def("black76_aux", &black76_aux, py::arg("F"), py::arg("K"), py::arg("T"), py::arg("sigma"),
        py::arg("df"), py::arg("side"), py::call_guard<py::gil_scoped_release>());
  m.def("black76_value_and_vega", &black76_value_and_vega, py::arg("F"), py::arg("K"), py::arg("T"),
        py::arg("sigma"), py::arg("df"), py::arg("side"), py::arg("sqrt_t") = -1.0,
        py::call_guard<py::gil_scoped_release>());
  m.def("black76_greeks", &black76_greeks, py::arg("F"), py::arg("K"), py::arg("T"),
        py::arg("sigma"), py::arg("r"), py::arg("df"), py::arg("side"),
        py::call_guard<py::gil_scoped_release>());
  m.def(
      "implied_vol",
      [](double price, double f, double k, double t, double df, Side side) {
        return atxvol::python::unwrap(implied_vol(price, f, k, t, df, side));
      },
      py::arg("price"), py::arg("F"), py::arg("K"), py::arg("T"), py::arg("df"), py::arg("side"),
      py::call_guard<py::gil_scoped_release>());

  m.def("black76_price_batch", &price_batch, py::arg("F"), py::arg("K"), py::arg("T"),
        py::arg("sigma"), py::arg("df"), py::arg("side"),
        "Vectorized Black-76 pricing over equal-length one-dimensional arrays.");
  m.def("implied_vol_batch", &iv_batch, py::arg("price"), py::arg("F"), py::arg("K"), py::arg("T"),
        py::arg("df"), py::arg("side"),
        "Vectorized implied-vol inversion.\n\n"
        "Returns ``(vols, status)``. A lane that cannot be inverted yields NaN in\n"
        "``vols`` and ``int(ErrorCode)`` in ``status``; a converged lane yields\n"
        "``STATUS_OK``. Only a malformed call (shape mismatch, wrong rank) raises.");

  m.def(
      "andersen_lake",
      [](double s, double k, double t, double sigma, double r, double q, Side side,
         const std::optional<AlOpts> &opts) {
        return atxvol::python::unwrap(andersen_lake(s, k, t, sigma, r, q, side, opts));
      },
      py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("sigma"), py::arg("r"),
      py::arg("q"), py::arg("side"), py::arg("opts") = std::nullopt,
      py::call_guard<py::gil_scoped_release>());
  m.def(
      "baw_american",
      [](double s, double k, double t, double sigma, double r, double q, Side side,
         std::uint16_t max_iter, double tol) {
        return atxvol::python::unwrap(baw_american(s, k, t, sigma, r, q, side, max_iter, tol));
      },
      py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("sigma"), py::arg("r"),
      py::arg("q"), py::arg("side"), py::arg("max_iter") = 16, py::arg("tol") = 1.0e-8,
      py::call_guard<py::gil_scoped_release>());
  m.def(
      "american_price",
      [](double s, double k, double t, double sigma, double r, double q, Side side,
         AmericanMethod method, const std::optional<AlOpts> &opts) {
        return atxvol::python::unwrap(american_price(s, k, t, sigma, r, q, side, method, opts));
      },
      py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("sigma"), py::arg("r"),
      py::arg("q"), py::arg("side"), py::arg("method") = AmericanMethod::AndersenLake,
      py::arg("opts") = std::nullopt, py::call_guard<py::gil_scoped_release>());
  // ── Y3: numpy-native American batch (NaN + per-lane status) ──────────────
  py::enum_<simd::SimdIsa>(m, "SimdIsa")
      .value("AUTO", simd::SimdIsa::Auto)
      .value("FORCE_SCALAR", simd::SimdIsa::ForceScalar)
      .value("FORCE_AVX2", simd::SimdIsa::ForceAvx2);

  m.def("american_price_batch", &american_price_batch_py, py::arg("S"), py::arg("K"),
        py::arg("T"), py::arg("sigma"), py::arg("r"), py::arg("q"), py::arg("side"),
        py::arg("method") = AmericanMethod::AndersenLake, py::arg("opts") = std::nullopt,
        py::arg("isa") = simd::SimdIsa::Auto,
        "Price a whole book of American options in one call.\n\n"
        "Returns ``(prices, status)``: the genuine early-exercise lanes are\n"
        "grouped into one homogeneous pack and every other lane patches through\n"
        "the exact scalar Andersen-Lake, so public output order is preserved.\n"
        "``status[i] == STATUS_OK`` unless the lane was outside the batch\n"
        "route's supported regime. A shape mismatch raises.\n\n"
        "``method`` and ``opts`` are honoured: the default engagement\n"
        "(Andersen-Lake, no ``AlOpts``) takes the laned route, and ANY other\n"
        "engagement takes the exact scalar ``american_price`` per lane under the\n"
        "same single GIL release — bit-identical to a loop over\n"
        "``american_price`` either way. ``isa`` selects the laned route's kernel\n"
        "and therefore has no effect on the non-default engagement.");

  m.def("american_greeks_batch", &american_greeks_batch_py, py::arg("S"), py::arg("K"),
        py::arg("T"), py::arg("sigma"), py::arg("r"), py::arg("q"), py::arg("side"),
        py::arg("analytic") = false, py::arg("isa") = simd::SimdIsa::Auto,
        "American Greeks for a whole book as numpy SoA columns.\n\n"
        "Returns a dict of delta/gamma/vega/theta/rho/vanna/volga/charm/price\n"
        "plus `status`. `analytic=False` (default) fans the scalar FD reference\n"
        "per lane; `analytic=True` takes the analytic route, which defers to FD\n"
        "off its supported regime.");

  m.def("american_implied_vol_batch", &american_iv_batch_py, py::arg("price"), py::arg("spot"),
        py::arg("K"), py::arg("T"), py::arg("r"), py::arg("q"), py::arg("side"),
        py::arg("method") = AmericanMethod::AndersenLake, py::arg("tol") = 1.0e-7,
        py::arg("max_iter") = 64, py::arg("opts") = std::nullopt,
        py::arg("warm_start_chain") = false,
        "Strike-axis American IV inversion; (spot, T, r, q, side) are shared.\n\n"
        "Returns ``(ivs, status)`` on the NaN + per-lane status convention: an\n"
        "uninvertible quote NaNs its own lane and leaves every other lane\n"
        "intact. Only a shape mismatch raises.");

  m.def("american_price_slice", &american_slice, py::arg("strikes"), py::arg("spot"), py::arg("T"),
        py::arg("sigma"), py::arg("r"), py::arg("q"), py::arg("side"),
        py::arg("opts") = std::nullopt,
        "Price one call or put strike slice while reusing an exercise boundary.");
  m.def(
      "american_delta",
      [](double s, double k, double t, double sigma, double r, double q, Side side,
         AmericanMethod method, const std::optional<AlOpts> &opts) {
        return atxvol::python::unwrap(american_delta(s, k, t, sigma, r, q, side, method, opts));
      },
      py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("sigma"), py::arg("r"),
      py::arg("q"), py::arg("side"), py::arg("method") = AmericanMethod::AndersenLake,
      py::arg("opts") = std::nullopt, py::call_guard<py::gil_scoped_release>());
  m.def(
      "american_greeks_fd",
      [](double s, double k, double t, double sigma, double r, double q, Side side,
         AmericanMethod method, const std::optional<AlOpts> &opts, bool warm_start) {
        return atxvol::python::unwrap(
            american_greeks_fd(s, k, t, sigma, r, q, side, method, opts, warm_start));
      },
      py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("sigma"), py::arg("r"),
      py::arg("q"), py::arg("side"), py::arg("method") = AmericanMethod::AndersenLake,
      py::arg("opts") = std::nullopt, py::arg("warm_start") = false,
      py::call_guard<py::gil_scoped_release>());
  m.def(
      "american_greeks_al",
      [](double s, double k, double t, double sigma, double r, double q, Side side,
         const std::optional<AlOpts> &opts) {
        return atxvol::python::unwrap(american_greeks_al(s, k, t, sigma, r, q, side, opts));
      },
      py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("sigma"), py::arg("r"),
      py::arg("q"), py::arg("side"), py::arg("opts") = std::nullopt,
      py::call_guard<py::gil_scoped_release>());
  m.def(
      "american_carry_greeks_al",
      [](double s, double k, double t, double sigma, double r, double q, Side side,
         const std::optional<AlOpts> &opts) {
        return atxvol::python::unwrap(american_carry_greeks_al(s, k, t, sigma, r, q, side, opts));
      },
      py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("sigma"), py::arg("r"),
      py::arg("q"), py::arg("side"), py::arg("opts") = std::nullopt,
      py::call_guard<py::gil_scoped_release>());
  m.def(
      "american_carry_greeks_fd",
      [](double s, double k, double t, double sigma, double r, double q, Side side,
         AmericanMethod method, const std::optional<AlOpts> &opts) {
        return atxvol::python::unwrap(
            american_carry_greeks_fd(s, k, t, sigma, r, q, side, method, opts));
      },
      py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("sigma"), py::arg("r"),
      py::arg("q"), py::arg("side"), py::arg("method") = AmericanMethod::AndersenLake,
      py::arg("opts") = std::nullopt, py::call_guard<py::gil_scoped_release>());
  // G4: early-exercise (critical) price B(T) + assignment-risk screen.
  m.def(
      "exercise_boundary",
      [](double k, double t, double sigma, double r, double q, Side side,
         const std::optional<AlOpts> &opts) {
        return atxvol::python::unwrap(exercise_boundary(k, t, sigma, r, q, side, opts));
      },
      py::arg("strike"), py::arg("T"), py::arg("sigma"), py::arg("r"), py::arg("q"),
      py::arg("side"), py::arg("opts") = std::nullopt, py::call_guard<py::gil_scoped_release>());
  m.def(
      "assignment_risk",
      [](double s, double k, double t, double sigma, double r, double q, Side side,
         const std::optional<AlOpts> &opts) {
        return atxvol::python::unwrap(assignment_risk(s, k, t, sigma, r, q, side, opts));
      },
      py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("sigma"), py::arg("r"),
      py::arg("q"), py::arg("side"), py::arg("opts") = std::nullopt,
      py::call_guard<py::gil_scoped_release>());
  m.def(
      "american_implied_vol",
      [](double price, double s, double k, double t, double r, double q, Side side,
         AmericanMethod method, double tol, std::uint16_t max_iter,
         const std::optional<AlOpts> &opts, double warm_start) {
        return atxvol::python::unwrap(american_implied_vol(price, s, k, t, r, q, side, method, tol,
                                                           max_iter, opts, nullptr, warm_start));
      },
      py::arg("price"), py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("r"),
      py::arg("q"), py::arg("side"), py::arg("method") = AmericanMethod::AndersenLake,
      py::arg("tol") = 1.0e-7, py::arg("max_iter") = 64, py::arg("opts") = std::nullopt,
      py::arg("warm_start") = 0.0, py::call_guard<py::gil_scoped_release>());
}
