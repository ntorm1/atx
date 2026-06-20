#pragma once

// atx_impl_test — APCA statistical factor-model builder for Phase 2 sizing
// (Task 2: PIT statistical factor-model builder).
//
// Two layers:
//   Math layer  — fit_stat_factor_model(R, cfg, fit_begin, fit_end)
//                 Pure APCA Connor-Korajczyk 2-pass on a complete-case N×T
//                 return matrix. Panel-independent so the PIT test can feed a
//                 poisoned copy without touching the Panel object.
//
//   Panel layer — build_stat_risk_model(panel, d, window, cfg)
//                 Causal gather of complete-case returns for [d-window+1, d],
//                 then delegates to fit_stat_factor_model.
//                 NEVER reads returns at index t > d (PIT-safe).
//
// Algorithm (Connor-Korajczyk 1986):
//   Pass 1 (equal-weighted):
//     R0 = copy of R; demean_rows(R0);
//     Fhat1 = apca_factor_returns(R0, K);
//     B1    = exposures(R0, Fhat1);
//     s1    = specific_variances(R0, B1, Fhat1);
//   Pass 2 (GLS, opt-in):
//     Rw    = gls_reweight(R0, s1);
//     Fhat  = apca_factor_returns(Rw, K);
//     B     = exposures(R0, Fhat);   // UN-weighted R — original scale
//     s     = specific_variances(R0, B, Fhat);
//   Factor covariance:
//     F = detail::factor_covariance(Fhat, -1.0);  // auto Ledoit-Wolf
//   Assembly:
//     return FactorModel::create(B, F, s, fit_begin, fit_end);

#include <cmath>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/core/linalg/linalg.hpp"    // MatX, VecX

#include "atx/engine/alpha/panel.hpp"    // Panel, FieldId
#include "atx/engine/risk/factor_model.hpp"     // FactorModel, detail::factor_covariance
#include "atx/engine/risk/stat_factor_model.hpp" // detail::{demean_rows, apca_factor_returns,
                                                  //   exposures, specific_variances, gls_reweight}

namespace atx_impl_test {

// Configuration for the statistical (APCA) factor model.
struct StatModelCfg {
  atx::usize K = 15;       // number of factors to extract
  bool gls_reweight = true; // run GLS pass 2 (Connor-Korajczyk)
};

// ===========================================================================
//  Math layer
//
//  Fit an APCA factor model on a COMPLETE-CASE N×T return matrix R
//  (row=asset, col=date, NO NaNs). Does not touch Panel.
//
//  Preconditions (returns Err if violated):
//    N > T, T > K, N > K
//  (The APCA T×T Gram trick requires N > T; the eigendecomposition requires
//   T > K so the top-K eigenvalues are well-separated; N > K is structural.)
//
//  Algorithm:
//    1. R0 = copy of R; demean_rows(R0)   (col-demean each asset row in place)
//    2. Pass 1 (equal-weighted):
//         Fhat1 = apca_factor_returns(R0, K)
//         B1    = exposures(R0, Fhat1)
//         s1    = specific_variances(R0, B1, Fhat1)
//    3. Pass 2 (GLS, if cfg.gls_reweight):
//         Rw   = gls_reweight(R0, s1)
//         Fhat = apca_factor_returns(Rw, K)
//         B    = exposures(R0, Fhat)      // UN-weighted R: original scale
//         s    = specific_variances(R0, B, Fhat)
//       else: Fhat=Fhat1, B=B1, s=s1
//    4. F = detail::factor_covariance(Fhat, -1.0)  (auto Ledoit-Wolf)
//    5. return FactorModel::create(B, F, s, fit_begin, fit_end)
// ===========================================================================
[[nodiscard]] inline atx::core::Result<atx::engine::risk::FactorModel>
fit_stat_factor_model(const atx::core::linalg::MatX &R, const StatModelCfg &cfg,
                      atx::usize fit_begin, atx::usize fit_end) {
  using namespace atx::engine::risk::detail;
  using atx::core::Err;
  using atx::core::ErrorCode;

  const atx::usize N = static_cast<atx::usize>(R.rows());
  const atx::usize T = static_cast<atx::usize>(R.cols());
  const atx::usize K = cfg.K;

  // Precondition checks (fail cleanly — no crash).
  if (N <= T) {
    return Err(ErrorCode::InvalidArgument,
               "fit_stat_factor_model: N <= T (APCA requires N > T); N=" +
                   std::to_string(N) + " T=" + std::to_string(T));
  }
  if (T <= K) {
    return Err(ErrorCode::InvalidArgument,
               "fit_stat_factor_model: T <= K; T=" + std::to_string(T) +
                   " K=" + std::to_string(K));
  }
  if (N <= K) {
    return Err(ErrorCode::InvalidArgument,
               "fit_stat_factor_model: N <= K; N=" + std::to_string(N) +
                   " K=" + std::to_string(K));
  }

  // Step 1: column-demean a working copy of R.
  atx::core::linalg::MatX R0 = R;
  demean_rows(R0);

  // Step 2: Pass 1 — equal-weighted APCA.
  ATX_TRY(atx::core::linalg::MatX Fhat1, apca_factor_returns(R0, K));
  ATX_TRY(atx::core::linalg::MatX B1, exposures(R0, Fhat1));
  atx::core::linalg::VecX s1 = specific_variances(R0, B1, Fhat1);

  // Step 3: Pass 2 — GLS reweight (opt-in).
  atx::core::linalg::MatX Fhat;
  atx::core::linalg::MatX B;
  atx::core::linalg::VecX s;
  if (cfg.gls_reweight) {
    const atx::core::linalg::MatX Rw = gls_reweight(R0, s1);
    ATX_TRY(Fhat, apca_factor_returns(Rw, K));
    ATX_TRY(B, exposures(R0, Fhat)); // UN-weighted R: original scale
    s = specific_variances(R0, B, Fhat);
  } else {
    Fhat = std::move(Fhat1);
    B = std::move(B1);
    s = std::move(s1);
  }

  // Step 4: factor covariance (Ledoit-Wolf auto intensity: cfg_shrink < 0).
  atx::core::linalg::MatX F = atx::engine::risk::detail::factor_covariance(Fhat, -1.0);

  // Step 5: assemble FactorModel.
  return atx::engine::risk::FactorModel::create(std::move(B), std::move(F), std::move(s),
                                                fit_begin, fit_end);
}

// ===========================================================================
//  Panel layer result.
// ===========================================================================
struct RiskModelResult {
  atx::engine::risk::FactorModel model; // over the active names (N rows)
  std::vector<atx::usize> active_inst;  // model row r -> panel instrument index
};

// ===========================================================================
//  Panel layer — causal gather overload taking an explicit returns span.
//
//  This overload lets the PIT test pass a POISONED copy of the returns span
//  (with future dates overwritten) and prove the build is unchanged, without
//  mutating the Panel object.
//
//  Active names: in_universe(d, i) AND returns[t*I + i] is finite for EVERY
//  t in [d-window+1, d] (complete-case).
//
//  Reads returns ONLY at date indices t in [d-window+1, d] — NEVER t > d.
//  fit_begin = d-window+1, fit_end = d+1.
//
//  Err if:
//    d < window-1                    (not enough history)
//    N_active <= T  (== window)      (too few names for APCA)
//    propagated from fit_stat_factor_model (T <= K, etc.)
// ===========================================================================
[[nodiscard]] inline atx::core::Result<RiskModelResult>
build_stat_risk_model_from_returns(const atx::engine::alpha::Panel &panel, atx::usize d,
                                   atx::usize window, const StatModelCfg &cfg,
                                   std::span<const atx::f64> returns_span) {
  using atx::core::Err;
  using atx::core::ErrorCode;

  const atx::usize I = panel.instruments();

  // Validate history depth. The explicit window==0 guard avoids unsigned underflow
  // in `window - 1` (which would wrap to SIZE_MAX).
  if (window == 0 || d < window - 1) {
    return Err(ErrorCode::InvalidArgument,
               "build_stat_risk_model: d < window-1 (insufficient history); d=" +
                   std::to_string(d) + " window=" + std::to_string(window));
  }
  // Causal window: [t0, d] inclusive, length T == window.
  const atx::usize t0 = d - window + 1; // earliest date index
  const atx::usize T = window;

  // --- Gather active instruments: in_universe at d AND complete returns over [t0, d]. ---
  // We check in_universe at date d (the rebalance date) as the universe predicate.
  // Complete-case: finite return at every t in [t0, d].
  std::vector<atx::usize> active_inst;
  active_inst.reserve(I);
  for (atx::usize i = 0; i < I; ++i) {
    if (!panel.in_universe(d, i)) {
      continue;
    }
    bool complete = true;
    for (atx::usize t = t0; t <= d; ++t) {
      const atx::f64 r = returns_span[t * I + i];
      if (!std::isfinite(r)) {
        complete = false;
        break;
      }
    }
    if (complete) {
      active_inst.push_back(i);
    }
  }

  const atx::usize N = active_inst.size();
  if (N <= T) {
    return Err(ErrorCode::InvalidArgument,
               "build_stat_risk_model: N_active <= T (too few names for APCA); N=" +
                   std::to_string(N) + " T=" + std::to_string(T));
  }

  // --- Build R (N×T): R(r, c) = returns[(t0+c)*I + active_inst[r]]. ---
  // Reads ONLY t in [t0, d] — causal, never t > d.
  atx::core::linalg::MatX R(static_cast<Eigen::Index>(N), static_cast<Eigen::Index>(T));
  for (atx::usize r = 0; r < N; ++r) {
    const atx::usize inst = active_inst[r];
    for (atx::usize c = 0; c < T; ++c) {
      R(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c)) =
          returns_span[(t0 + c) * I + inst];
    }
  }

  // fit_begin = t0, fit_end = d+1 (exclusive upper bound).
  ATX_TRY(atx::engine::risk::FactorModel model,
          fit_stat_factor_model(R, cfg, t0, d + 1));

  return atx::core::Ok(RiskModelResult{std::move(model), std::move(active_inst)});
}

// ===========================================================================
//  Panel layer — primary entry point.
//
//  Causal gather + fit. Delegates to the span overload using the panel's own
//  returns field span. Err if the "returns" field is absent.
// ===========================================================================
[[nodiscard]] inline atx::core::Result<RiskModelResult>
build_stat_risk_model(const atx::engine::alpha::Panel &panel, atx::usize d, atx::usize window,
                      const StatModelCfg &cfg) {
  using atx::core::Err;
  using atx::core::ErrorCode;

  // Resolve the "returns" field.
  auto returns_id = panel.field_id("returns");
  if (!returns_id) {
    return Err(ErrorCode::NotFound,
               "build_stat_risk_model: panel has no 'returns' field");
  }
  const std::span<const atx::f64> returns_span =
      panel.field_all(*returns_id);

  return build_stat_risk_model_from_returns(panel, d, window, cfg, returns_span);
}

} // namespace atx_impl_test
