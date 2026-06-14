#include <functional>
#include <string>

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>

#include "atx/core/decimal.hpp"
#include "atx/core/types.hpp"
#include "result.hpp"

namespace py = pybind11;
using atx::core::Decimal;

namespace {

// Guard from_int: C++ ABORTS when |whole| > kMaxWhole. Raise instead so Python
// callers get an exception rather than a killed interpreter.
Decimal decimal_from_int_guarded(atx::i64 whole) {
  if (whole > Decimal::kMaxWhole || whole < -Decimal::kMaxWhole) {
    throw py::value_error("Decimal.from_int out of range (|whole| > kMaxWhole)");
  }
  return Decimal::from_int(whole);
}

void bind_decimal(py::module_ &m) {
  py::class_<Decimal>(m, "Decimal", "Exact fixed-point money (nano scale, 1e-9).")
      .def_static("from_raw", &Decimal::from_raw, py::arg("mantissa"),
                  "Exact construction from a raw i64 mantissa (value = mantissa / 1e9).")
      .def_static("from_int", &decimal_from_int_guarded, py::arg("whole"),
                  "Whole-unit construction; raises on |whole| > kMaxWhole.")
      .def_static(
          "from_double", [](atx::f64 v) { return atxpy::unwrap(Decimal::from_double(v)); },
          py::arg("value"), "Round a double to the nano grid; raises on NaN/inf/range.")
      .def_static(
          "from_string", [](const std::string &s) { return atxpy::unwrap(Decimal::from_string(s)); },
          py::arg("text"), "Parse a decimal string; raises AtxError on bad input.")
      .def("raw", &Decimal::raw, "Raw i64 mantissa (value * 1e9).")
      .def("to_double", &Decimal::to_double, "Lossy double (display/heuristics only).")
      .def("to_string", &Decimal::to_string, "Canonical decimal string.")
      .def("round", &Decimal::round, "Round half-away-from-zero to the nearest whole unit.")
      .def(py::self + py::self)
      .def(py::self - py::self)
      .def(py::self * py::self)
      .def(py::self / py::self)
      .def(-py::self)
      .def(py::self == py::self)
      .def(py::self != py::self)
      .def(py::self < py::self)
      .def(py::self <= py::self)
      .def(py::self > py::self)
      .def(py::self >= py::self)
      .def("__float__", &Decimal::to_double)
      .def("__str__", &Decimal::to_string)
      .def("__repr__", [](const Decimal &d) { return "Decimal('" + d.to_string() + "')"; })
      .def("__hash__", [](const Decimal &d) { return std::hash<atx::i64>{}(d.raw()); });
}

} // namespace

// Defined in bind_symbol.cpp.
void bind_symbol(py::module_ &m);

void bind_core(py::module_ &m) {
  bind_decimal(m);
  bind_symbol(m);
}
