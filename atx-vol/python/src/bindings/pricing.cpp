#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/american.hpp"
#include "atx/vol/american_iv.hpp"
#include "atx/vol/batch.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/implied_vol.hpp"
#include "atx/vol/types.hpp"
#include "result.hpp"

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
  {
    py::gil_scoped_release release;
    atxvol::python::unwrap(black76_price_batch(
        f, k, t, sigma, df, sides,
        std::span<double>{output.mutable_data(), static_cast<std::size_t>(output.size())}));
  }
  return output;
}

py::array_t<double> iv_batch(const DoubleArray &price_array, const DoubleArray &f_array,
                             const DoubleArray &k_array, const DoubleArray &t_array,
                             const DoubleArray &df_array, Side side) {
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
  {
    py::gil_scoped_release release;
    atxvol::python::unwrap(implied_vol_batch(
        price, f, k, t, df, sides,
        std::span<double>{output.mutable_data(), static_cast<std::size_t>(output.size())},
        statuses));
  }
  for (std::size_t i = 0; i < statuses.size(); ++i) {
    if (!statuses[i]) {
      const auto &error = statuses[i].error();
      throw atxvol::python::AtxException(atx::core::Error{
          error.code(), "batch lane " + std::to_string(i) + ": " + error.message()});
    }
  }
  return output;
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

  py::class_<AloPricer>(m, "AloPricer")
      .def(py::init<double, double, double, double, double, Side, const std::optional<AlOpts> &>(),
           py::arg("spot"), py::arg("strike"), py::arg("T"), py::arg("r"), py::arg("q"),
           py::arg("side"), py::arg("opts") = std::nullopt)
      .def("price", &AloPricer::price, py::arg("sigma"), py::call_guard<py::gil_scoped_release>());

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
        "Vectorized implied-vol inversion; raises AtxError with the failing lane.");

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
