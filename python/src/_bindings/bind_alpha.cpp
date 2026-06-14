#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "result.hpp"

namespace py = pybind11;
namespace alpha = atx::engine::alpha;

namespace {

// Parse + analyze + compile a single expression with a throwaway Library.
// The Program is self-contained (owns its code/fields/roots), so the local
// Library and intermediate Ast/Analysis can die when this returns.
alpha::Program compile_one(const std::string &src) {
  alpha::Library lib;
  alpha::Ast ast = atxpy::unwrap(alpha::parse_expr(src, lib));
  alpha::Analysis an = atxpy::unwrap(alpha::analyze(ast));
  return atxpy::unwrap(alpha::compile(ast, an));
}

void bind_alpha(py::module_ &m) {
  py::class_<alpha::Library>(m, "Library", "Operator registry (default ctor registers built-ins).")
      .def(py::init<>())
      .def("size", &alpha::Library::size);

  // Ast / Analysis are opaque handles produced by the pipeline.
  py::class_<alpha::Ast>(m, "Ast", "Parsed alpha AST (borrows its Library).");
  py::class_<alpha::Analysis>(m, "Analysis", "Type-check result for an Ast.")
      .def_property_readonly("required_lookback",
                             [](const alpha::Analysis &a) { return a.required_lookback(); });

  py::class_<alpha::Program>(m, "Program", "Compiled alpha bytecode (self-contained).")
      .def_property_readonly("required_lookback",
                             [](const alpha::Program &p) { return p.required_lookback; })
      .def_property_readonly("num_slots", [](const alpha::Program &p) { return p.num_slots; })
      .def_readonly("fields", &alpha::Program::fields)
      .def_property_readonly("roots",
                             [](const alpha::Program &p) {
                               std::vector<std::string> names;
                               names.reserve(p.roots.size());
                               for (const alpha::Program::Root &r : p.roots) {
                                 names.push_back(r.name);
                               }
                               return names;
                             })
      .def("cache_hit_pct", &alpha::Program::cache_hit_pct);

  // The DSL pipeline, exposed step-by-step. parse_expr returns an Ast that
  // borrows the Library; keep_alive<0,2> ties the Ast's lifetime to the lib arg.
  m.def(
      "parse_expr",
      [](const std::string &src, const alpha::Library &lib) {
        return atxpy::unwrap(alpha::parse_expr(src, lib));
      },
      py::arg("source"), py::arg("library"), py::keep_alive<0, 2>(),
      "Parse a DSL expression into an Ast (raises AtxError on a parse error).");
  m.def(
      "analyze", [](const alpha::Ast &ast) { return atxpy::unwrap(alpha::analyze(ast)); },
      py::arg("ast"), "Type-check an Ast (raises AtxError on a type error).");
  m.def(
      "compile",
      [](const alpha::Ast &ast, const alpha::Analysis &an) {
        return atxpy::unwrap(alpha::compile(ast, an));
      },
      py::arg("ast"), py::arg("analysis"), "Compile an analyzed Ast into a Program.");
  m.def("compile_alpha", &compile_one, py::arg("source"),
        "One-shot parse+analyze+compile of an expression into a Program.");

  m.def(
      "evaluate_program",
      [](const alpha::Program &prog, const std::map<std::string, py::array_t<double>> &fields) {
        if (fields.empty()) {
          throw py::value_error("need at least one field column");
        }
        const auto &first = fields.begin()->second;
        if (first.ndim() != 2) {
          throw py::value_error("each field must be a 2D [dates, instruments] array");
        }
        const atx::usize dates = static_cast<atx::usize>(first.shape(0));
        const atx::usize inst = static_cast<atx::usize>(first.shape(1));
        std::vector<std::string> names;
        std::vector<std::vector<double>> cols;
        names.reserve(fields.size());
        cols.reserve(fields.size());
        for (const auto &kv : fields) {
          auto a = kv.second.unchecked<2>();
          if (static_cast<atx::usize>(a.shape(0)) != dates ||
              static_cast<atx::usize>(a.shape(1)) != inst) {
            throw py::value_error("all field arrays must share the same [dates, instruments] shape");
          }
          std::vector<double> col(dates * inst);
          for (atx::usize d = 0; d < dates; ++d) {
            for (atx::usize j = 0; j < inst; ++j) {
              col[d * inst + j] = a(static_cast<py::ssize_t>(d), static_cast<py::ssize_t>(j));
            }
          }
          names.push_back(kv.first);
          cols.push_back(std::move(col));
        }
        alpha::Panel panel =
            atxpy::unwrap(alpha::Panel::create(dates, inst, names, cols, std::vector<std::uint8_t>{}));
        alpha::Engine engine{panel};
        alpha::SignalSet sigs = atxpy::unwrap(engine.evaluate(prog));
        py::array_t<double> out(
            {static_cast<py::ssize_t>(dates), static_cast<py::ssize_t>(inst)});
        auto v = out.mutable_unchecked<2>();
        const std::vector<double> &vals = sigs.alphas.at(0).values; // date-major
        for (atx::usize d = 0; d < dates; ++d) {
          for (atx::usize j = 0; j < inst; ++j) {
            v(static_cast<py::ssize_t>(d), static_cast<py::ssize_t>(j)) = vals[d * inst + j];
          }
        }
        return out;
      },
      py::arg("program"), py::arg("fields"),
      "Evaluate root 0 of a Program over {field: 2D [dates,instruments]} -> (dates,instruments).");
}

} // namespace

void bind_alpha_module(py::module_ &m) { bind_alpha(m); }
