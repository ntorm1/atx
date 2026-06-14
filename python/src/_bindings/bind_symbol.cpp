#include <functional>
#include <string>
#include <string_view>

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>

#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"

namespace py = pybind11;
using atx::core::domain::Symbol;
using atx::core::domain::SymbolTable;

void bind_symbol(py::module_ &m) {
  py::class_<Symbol>(m, "Symbol", "Opaque interned instrument id (32-bit).")
      .def(py::init([](atx::u32 id) { return Symbol{id}; }), py::arg("id"))
      .def_readonly("id", &Symbol::id)
      .def(py::self == py::self)
      .def(py::self != py::self)
      .def(py::self < py::self)
      .def("__hash__", [](const Symbol &s) { return std::hash<atx::u32>{}(s.id); })
      .def("__repr__", [](const Symbol &s) { return "Symbol(" + std::to_string(s.id) + ")"; });

  py::class_<SymbolTable>(m, "SymbolTable", "Owns the id<->name interning map (move-only).")
      .def(py::init<>())
      .def(
          "intern", [](SymbolTable &t, std::string_view name) { return t.intern(name); },
          py::arg("name"), "Intern a name to a stable Symbol (idempotent).")
      .def(
          "name",
          [](const SymbolTable &t, Symbol s) -> std::string {
            if (s.id >= t.size()) {
              throw py::value_error("Symbol not from this table (id >= size)");
            }
            return std::string{t.name(s)};
          },
          py::arg("symbol"), "Name for a Symbol from this table; raises if out of range.")
      .def("size", &SymbolTable::size, "Number of distinct interned symbols.")
      .def("__len__", &SymbolTable::size);
}
