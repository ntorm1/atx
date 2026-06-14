#pragma once

// atx::engine::data — FactorModelArtifact: a BYO (bring-your-own) factored
// covariance block (S6.6).
//
// Lives in data:: (NOT risk::) to avoid a risk→data include edge. All matrices
// are owned by value. Validation is fully delegated to risk::FactorModel::create
// (via artifact_to_factor_model in adapt_factor.hpp) — the single source of
// truth for shape / SPD / fit-window checks.

#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // usize

namespace atx::engine::data {

// A BYO factor model block. Owns its matrices by value.
//
//   X  — M×K exposure matrix
//   F  — K×K factor covariance (must be SPD; validated on lowering)
//   D  — M specific variances (must be > 0; validated on lowering)
//   fit_begin / fit_end — the fit window [fit_begin, fit_end) forwarded to
//                         risk::FactorModel::create (require fit_begin < fit_end)
//
// Construct this struct directly (aggregate init) and pass to
// artifact_to_factor_model to produce a risk::FactorModel.
struct FactorModelArtifact {
  atx::core::linalg::MatX X;    // M×K exposures
  atx::core::linalg::MatX F;    // K×K factor covariance (must be SPD)
  atx::core::linalg::VecX D;    // M specific variances (must be > 0)
  atx::usize fit_begin = 0;
  atx::usize fit_end   = 0;
};

} // namespace atx::engine::data
