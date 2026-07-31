#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/batch.hpp"
#include "atx/vol/surface.hpp"
#include "atx/vol/vol_surface.hpp"
#include "result.hpp"

namespace py = pybind11;
using namespace atx::vol;

namespace {

using DoubleArray = py::array_t<double, py::array::c_style | py::array::forcecast>;

py::array_t<double> essvi_batch(const EssviSlice &slice, const DoubleArray &k_array) {
  if (k_array.ndim() != 1) {
    throw py::value_error("k_log must be a one-dimensional array");
  }
  const auto k = std::span<const double>{k_array.data(), static_cast<std::size_t>(k_array.size())};
  py::array_t<double> output(k_array.size());
  {
    py::gil_scoped_release release;
    static_cast<void>(atxvol::python::unwrap(essvi_w_batch(
        slice, k,
        std::span<double>{output.mutable_data(), static_cast<std::size_t>(output.size())})));
  }
  return output;
}

} // namespace

void bind_surface(py::module_ &m) {
  py::class_<SviSlice>(m, "SviSlice")
      .def(py::init<>())
      .def(py::init<double, double, double, double, double, double>(), py::arg("a"), py::arg("b"),
           py::arg("rho"), py::arg("m"), py::arg("sigma"), py::arg("T"))
      .def_readwrite("a", &SviSlice::a)
      .def_readwrite("b", &SviSlice::b)
      .def_readwrite("rho", &SviSlice::rho)
      .def_readwrite("m", &SviSlice::m)
      .def_readwrite("sigma", &SviSlice::sigma)
      .def_readwrite("T", &SviSlice::T);

  py::class_<EssviSlice>(m, "EssviSlice")
      .def(py::init<>())
      .def(py::init<double, double, double, double>(), py::arg("theta"), py::arg("phi"),
           py::arg("rho"), py::arg("T"))
      .def_readwrite("theta", &EssviSlice::theta)
      .def_readwrite("phi", &EssviSlice::phi)
      .def_readwrite("rho", &EssviSlice::rho)
      .def_readwrite("T", &EssviSlice::T);

  py::class_<EssviGrad>(m, "EssviGrad")
      .def_readonly("dtheta", &EssviGrad::dtheta)
      .def_readonly("dphi", &EssviGrad::dphi)
      .def_readonly("drho", &EssviGrad::drho);

  py::class_<SviSurface>(m, "SviSurface")
      .def(py::init<std::size_t>(), py::arg("capacity"))
      .def(
          "set_slice",
          [](SviSurface &self, std::size_t index, const SviSlice &slice) {
            atxvol::python::unwrap(self.set_slice(index, slice));
          },
          py::arg("index"), py::arg("slice"))
      .def_property_readonly("n_slices", &SviSurface::n_slices)
      .def_property_readonly("capacity", &SviSurface::capacity)
      .def("w", &SviSurface::w, py::arg("k_log"), py::arg("T"),
           py::call_guard<py::gil_scoped_release>())
      .def("iv", &SviSurface::iv, py::arg("k_log"), py::arg("T"),
           py::call_guard<py::gil_scoped_release>());

  py::class_<EssviSurface>(m, "EssviSurface")
      .def(py::init<std::size_t>(), py::arg("capacity"))
      .def(
          "set_slice",
          [](EssviSurface &self, std::size_t index, const EssviSlice &slice) {
            atxvol::python::unwrap(self.set_slice(index, slice));
          },
          py::arg("index"), py::arg("slice"))
      .def_property_readonly("n_slices", &EssviSurface::n_slices)
      .def_property_readonly("capacity", &EssviSurface::capacity)
      .def("w", &EssviSurface::w, py::arg("k_log"), py::arg("T"),
           py::call_guard<py::gil_scoped_release>())
      .def("iv", &EssviSurface::iv, py::arg("k_log"), py::arg("T"),
           py::call_guard<py::gil_scoped_release>());

  m.def("svi_w", &svi_w, py::arg("slice"), py::arg("k_log"),
        py::call_guard<py::gil_scoped_release>());
  m.def("essvi_w", &essvi_w, py::arg("slice"), py::arg("k_log"),
        py::call_guard<py::gil_scoped_release>());
  m.def("essvi_w_grad", &essvi_w_grad, py::arg("slice"), py::arg("k_log"),
        py::call_guard<py::gil_scoped_release>());
  m.def("essvi_w_batch", &essvi_batch, py::arg("slice"), py::arg("k_log"));

  py::enum_<Parametrization>(m, "Parametrization")
      .value("ESSVI", Parametrization::Essvi)
      .value("SVI", Parametrization::Svi)
      .value("WING", Parametrization::Wing)
      .value("SVI_MM", Parametrization::SviMm)
      .value("C8", Parametrization::C8)
      .value("CSTAR16M", Parametrization::CStar16M);
  py::enum_<ResidualBasisKind>(m, "ResidualBasisKind")
      .value("NONE", ResidualBasisKind::None)
      .value("HINGE_QUAD", ResidualBasisKind::HingeQuad)
      .value("C2_BSPLINE", ResidualBasisKind::C2Bspline)
      .value("CHEBYSHEV", ResidualBasisKind::Chebyshev)
      .value("WING_BSPLINE", ResidualBasisKind::WingBspline)
      .value("FENGLER", ResidualBasisKind::Fengler);

  py::class_<EssviParams>(m, "EssviParams")
      .def(py::init<>())
      .def_readwrite("theta", &EssviParams::theta)
      .def_readwrite("phi", &EssviParams::phi)
      .def_readwrite("rho", &EssviParams::rho)
      .def_readwrite("rho_R", &EssviParams::rho_R)
      .def_readwrite("rho_scale", &EssviParams::rho_scale)
      .def_readwrite("psi", &EssviParams::psi)
      .def_readwrite("p", &EssviParams::p)
      .def_readwrite("lambda_", &EssviParams::lambda)
      .def_readwrite("lambda_R", &EssviParams::lambda_R)
      .def_readwrite("T", &EssviParams::T)
      .def_readwrite("F", &EssviParams::F)
      .def_readwrite("expiry_ns", &EssviParams::expiry_ns)
      .def_readwrite("expiry_id", &EssviParams::expiry_id)
      .def_readwrite("resid_coef", &EssviParams::resid_coef)
      .def_readwrite("resid_scale", &EssviParams::resid_scale)
      .def_readwrite("resid_basis_kind", &EssviParams::resid_basis_kind)
      .def_readwrite("resid_n_basis", &EssviParams::resid_n_basis);

  py::class_<SviParams>(m, "SviParams")
      .def(py::init<>())
      .def_readwrite("a", &SviParams::a)
      .def_readwrite("b", &SviParams::b)
      .def_readwrite("rho", &SviParams::rho)
      .def_readwrite("m", &SviParams::m)
      .def_readwrite("sigma", &SviParams::sigma)
      .def_readwrite("T", &SviParams::T)
      .def_readwrite("F", &SviParams::F)
      .def_readwrite("expiry_ns", &SviParams::expiry_ns)
      .def_readwrite("expiry_id", &SviParams::expiry_id);

  py::class_<EssviNatural>(m, "EssviNatural")
      .def_readonly("theta", &EssviNatural::theta)
      .def_readonly("phi", &EssviNatural::phi)
      .def_readonly("rho", &EssviNatural::rho);
  py::class_<EssviCube>(m, "EssviCube")
      .def_readonly("psi", &EssviCube::psi)
      .def_readonly("p", &EssviCube::p)
      .def_readonly("lambda_", &EssviCube::lambda);

  py::class_<VolSurface::Diagnostics>(m, "SurfaceDiagnostics")
      .def(py::init<>())
      .def_readwrite("rmse_vol", &VolSurface::Diagnostics::rmse_vol)
      .def_readwrite("max_residual_vol", &VolSurface::Diagnostics::max_residual_vol)
      .def_readwrite("n_quotes_used", &VolSurface::Diagnostics::n_quotes_used)
      .def_readwrite("n_quotes_dropped", &VolSurface::Diagnostics::n_quotes_dropped);

  py::class_<VolSurface>(m, "VolSurface")
      .def(py::init([](std::uint32_t uid, Parametrization param, std::size_t capacity) {
             return atxvol::python::unwrap(VolSurface::create(uid, param, capacity));
           }),
           py::arg("uid"), py::arg("parametrization"), py::arg("capacity"))
      .def(
          "set_slice_essvi",
          [](VolSurface &self, std::size_t index, const EssviParams &slice) {
            atxvol::python::unwrap(self.set_slice_essvi(index, slice));
          },
          py::arg("index"), py::arg("slice"))
      .def(
          "set_slice_svi",
          [](VolSurface &self, std::size_t index, const SviParams &slice) {
            atxvol::python::unwrap(self.set_slice_svi(index, slice));
          },
          py::arg("index"), py::arg("slice"))
      .def_property_readonly("parametrization", &VolSurface::param)
      .def_property_readonly("uid", &VolSurface::uid)
      .def_property("fit_ts_ns", &VolSurface::fit_ts_ns, &VolSurface::set_fit_ts_ns)
      .def_property_readonly("n_slices", &VolSurface::n_slices)
      .def_property_readonly("capacity", &VolSurface::capacity)
      .def_property(
          "diagnostics", [](const VolSurface &self) { return self.diagnostics(); },
          [](VolSurface &self, const VolSurface::Diagnostics &diag) { self.set_diagnostics(diag); })
      .def_property_readonly("essvi_slices",
                             [](const VolSurface &self) {
                               const auto slices = self.essvi_slices();
                               return std::vector<EssviParams>{slices.begin(), slices.end()};
                             })
      .def_property_readonly("svi_slices",
                             [](const VolSurface &self) {
                               const auto slices = self.svi_slices();
                               return std::vector<SviParams>{slices.begin(), slices.end()};
                             })
      .def("w", &VolSurface::w, py::arg("k_log"), py::arg("T"),
           py::call_guard<py::gil_scoped_release>())
      .def("iv", &VolSurface::iv, py::arg("k_log"), py::arg("T"),
           py::call_guard<py::gil_scoped_release>())
      .def("find_exact_T", &VolSurface::find_exact_T, py::arg("T"),
           py::call_guard<py::gil_scoped_release>())
      .def("iv_on_slice", &VolSurface::iv_on_slice, py::arg("slice_index"), py::arg("k_log"),
           py::call_guard<py::gil_scoped_release>());

  m.def("essvi_backbone_w", &essvi_backbone_w, py::arg("slice"), py::arg("k_log"));
  m.def("essvi_residual_w", &essvi_residual_w, py::arg("slice"), py::arg("k_log"));
  m.def("essvi_total_w", &essvi_total_w, py::arg("slice"), py::arg("k_log"));
  m.def("essvi_w_grad3", &essvi_w_grad3, py::arg("slice"), py::arg("k_log"));
  m.def("essvi_w_grad4", &essvi_w_grad4, py::arg("slice"), py::arg("k_log"));
  m.def("svi_total_w", &svi_total_w, py::arg("slice"), py::arg("k_log"));
  m.def("essvi_reparam_to_natural", &essvi_reparam_to_natural, py::arg("psi"), py::arg("p"),
        py::arg("lambda_"), py::arg("T"));
  m.def("essvi_natural_to_reparam", &essvi_natural_to_reparam, py::arg("theta"), py::arg("phi"),
        py::arg("rho"), py::arg("T"));
  m.def("essvi_phi_max", &essvi_phi_max, py::arg("theta"), py::arg("rho"));
  m.def("essvi_rho_from_lambda", &essvi_rho_from_lambda, py::arg("lambda_"));
  m.def("essvi_lambda_from_rho", &essvi_lambda_from_rho, py::arg("rho"));
}
