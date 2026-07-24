#include <exception>

#include <pybind11/pybind11.h>

#include "atx/vol/version.hpp"
#include "result.hpp"

namespace py = pybind11;

void bind_pricing(py::module_ &m);
void bind_surface(py::module_ &m);
void bind_surface_db(py::module_ &m);

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
  bind_pricing(m);
  bind_surface(m);
  bind_surface_db(m);
}
