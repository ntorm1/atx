#include <exception>

#include <pybind11/pybind11.h>

#include "result.hpp"

namespace py = pybind11;

// Each engine module registers its bindings through one of these registrars.
void bind_core(py::module_ &m);

PYBIND11_MODULE(_core, m) {
  m.doc() = "atxpy._core - thin pybind11 bindings over atx-engine.";

  // atxpy.AtxError: every atx::core::Error surfaces here.
  static py::exception<atxpy::AtxException> exc(m, "AtxError");
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) {
        std::rethrow_exception(p);
      }
    } catch (const atxpy::AtxException &e) {
      py::set_error(exc, e.what());
    }
  });

  bind_core(m);
}
