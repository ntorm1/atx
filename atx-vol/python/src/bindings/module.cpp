#include <exception>

#include <pybind11/pybind11.h>

#include "atx/vol/api/core/version.hpp"
#include "result.hpp"

namespace py = pybind11;

void bind_pricing(py::module_ &m);
void bind_surface(py::module_ &m);
void bind_surface_db(py::module_ &m);
void bind_strategy(py::module_ &m);
void bind_backtest(py::module_ &m);
void bind_analytics(py::module_ &m);
void bind_dispersion(py::module_ &m);
void bind_fit(py::module_ &m);

PYBIND11_MODULE(_core, m) {
  m.doc() = "pybind11 bindings for atx-vol";

  // PY-1: the raised exception carries the STRUCTURED code, not just prose.
  // `AtxException` holds `atx::core::ErrorCode` and the module binds that enum,
  // so programmatic dispatch (retry on Unavailable, skip on NotFound) can read
  // `err.code` instead of regex-matching a message that is not a contract. The
  // message itself is unchanged — it still leads with the code's name.
  static py::exception<atxvol::python::AtxException> exc(m, "AtxError");
  exc.attr("code") = py::none(); // class-level default for hand-constructed instances
  py::register_exception_translator([](std::exception_ptr ptr) {
    try {
      if (ptr) {
        std::rethrow_exception(ptr);
      }
    } catch (const atxvol::python::AtxException &error) {
      // `py::exception` shadows object_api::operator() with the deprecated
      // set-error overload, so go through a plain object to CONSTRUCT one.
      py::object exc_type = py::reinterpret_borrow<py::object>(py::handle(exc));
      py::object instance = exc_type(error.what());
      instance.attr("code") = py::cast(error.code());
      py::set_error(exc, instance);
    }
  });

  m.attr("__version__") = atx::vol::version();
  // Order matters: a py::arg default value is converted at definition time, so
  // every enum/struct used as a default must already be registered. Notably
  // `bind_backtest` registers QueryExecution, which the PricedSurface query
  // methods in `bind_surface_db` take as a default argument.
  bind_pricing(m);
  bind_surface(m);
  bind_backtest(m);
  bind_strategy(m);
  bind_surface_db(m);
  bind_analytics(m);
  // Last: `bind_dispersion` defaults a py::arg to DispersionBacktestConfig{},
  // whose nested RunConfig must already be registered by `bind_backtest`, and
  // subclasses the IStrategy that `bind_strategy` registers.
  bind_dispersion(m);
  // Last: `bind_fit` defaults a py::arg to PricerConfig{}, whose QueryPricingTier
  // is registered by `bind_backtest`, and it returns PricedSurface / AmericanGreeks
  // registered by `bind_surface_db` / `bind_pricing`.
  bind_fit(m);
}
