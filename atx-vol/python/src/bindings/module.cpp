#include <exception>

#include <pybind11/pybind11.h>

#include "atx/vol/version.hpp"
#include "result.hpp"

namespace py = pybind11;

void bind_pricing(py::module_ &m);
void bind_surface(py::module_ &m);
void bind_surface_db(py::module_ &m);
void bind_strategy(py::module_ &m);
void bind_backtest(py::module_ &m);
void bind_analytics(py::module_ &m);
void bind_dispersion(py::module_ &m);

PYBIND11_MODULE(_core, m) {
  m.doc() = "pybind11 bindings for atx-vol";

  static py::exception<atxvol::python::AtxException> exc(m, "AtxError");
  py::register_exception_translator([](std::exception_ptr ptr) {
    try {
      if (ptr) {
        std::rethrow_exception(ptr);
      }
    } catch (const atxvol::python::AtxException &error) {
      py::set_error(exc, error.what());
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
}
