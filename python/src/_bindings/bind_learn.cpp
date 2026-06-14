#include <vector>

#include <pybind11/eigen.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/learn/elastic_net.hpp"
#include "atx/engine/learn/feature_matrix.hpp"
#include "atx/engine/learn/latent.hpp"
#include "atx/engine/learn/learned_source.hpp"
#include "atx/engine/learn/linear_alpha.hpp"

namespace py = pybind11;
namespace learn = atx::engine::learn;

namespace {

void bind_learn(py::module_ &m) {
  py::class_<learn::ElasticNetCfg>(m, "ElasticNetCfg", "Elastic-net penalty config.")
      .def(py::init<>())
      .def_readwrite("lambda_", &learn::ElasticNetCfg::lambda)
      .def_readwrite("alpha", &learn::ElasticNetCfg::alpha)
      .def_readwrite("max_iter", &learn::ElasticNetCfg::max_iter)
      .def_readwrite("tol", &learn::ElasticNetCfg::tol);

  m.def(
      "elastic_net",
      [](const atx::core::linalg::MatX &X, const atx::core::linalg::VecX &y,
         const learn::ElasticNetCfg &c) { return learn::elastic_net(X, y, c); },
      py::arg("X"), py::arg("y"), py::arg("config"),
      "Deterministic elastic-net regression: standardized X (n x p), y (n) -> coefficients (p).");

  py::class_<learn::FeatureMatrix>(m, "FeatureMatrix", "Training rows: X (n x p) + per-horizon Y.")
      .def(py::init<>())
      .def_readwrite("n_dates", &learn::FeatureMatrix::n_dates)
      .def_readwrite("n_instruments", &learn::FeatureMatrix::n_instruments)
      .def_readwrite("n_features", &learn::FeatureMatrix::n_features)
      .def_readwrite("row_date", &learn::FeatureMatrix::row_date)
      .def_readwrite("row_inst", &learn::FeatureMatrix::row_inst)
      .def_readwrite("X", &learn::FeatureMatrix::X)
      .def_readwrite("Y", &learn::FeatureMatrix::Y)
      .def_readwrite("row_valid", &learn::FeatureMatrix::row_valid)
      .def("n_rows", &learn::FeatureMatrix::n_rows);

  py::class_<learn::LatentAugmentation>(m, "LatentAugmentation",
                                        "Optional PCA/interaction augmentation (empty by default).")
      .def(py::init<>());

  py::class_<learn::LinearAlphaCfg>(m, "LinearAlphaCfg")
      .def(py::init<>())
      .def_readwrite("en", &learn::LinearAlphaCfg::en)
      .def_readwrite("use_ridge_baseline", &learn::LinearAlphaCfg::use_ridge_baseline)
      .def_readwrite("cpcv", &learn::LinearAlphaCfg::cpcv)
      .def_readwrite("master_seed", &learn::LinearAlphaCfg::master_seed)
      .def_readwrite("horizons", &learn::LinearAlphaCfg::horizons);

  py::class_<learn::LearnedModel>(m, "LearnedModel", "A fitted ML alpha (coeffs per horizon).")
      .def_readonly("horizons", &learn::LearnedModel::horizons)
      .def_readonly("blend_w", &learn::LearnedModel::blend_w)
      .def_readonly("trial_count", &learn::LearnedModel::trial_count);

  m.def(
      "fit_linear",
      [](const learn::FeatureMatrix &fm, const learn::LatentAugmentation &aug,
         const learn::LinearAlphaCfg &cfg) { return learn::fit_linear(fm, aug, cfg); },
      py::arg("features"), py::arg("augmentation"), py::arg("config"),
      "Fit an elastic-net ML alpha over a FeatureMatrix with CPCV horizon blending.");

  m.def(
      "predict_at",
      [](const learn::LearnedModel &model, const learn::FeatureMatrix &fm, atx::usize date) {
        return learn::predict_at(model, fm, date);
      },
      py::arg("model"), py::arg("features"), py::arg("date"),
      "Horizon-blended prediction vector for the in-universe instruments at `date`.");

  m.def(
      "oos_deflated_sharpe",
      [](const learn::LearnedModel &model, const learn::FeatureMatrix &fm) {
        return learn::oos_deflated_sharpe(model, fm);
      },
      py::arg("model"), py::arg("features"),
      "Out-of-sample deflated Sharpe of the fitted model (deflation gate).");
}

} // namespace

void bind_learn_module(py::module_ &m) { bind_learn(m); }
