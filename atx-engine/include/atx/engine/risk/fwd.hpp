#pragma once

// atx::engine::risk — Barra-style factor risk model + risk-aware optimizer forward
// declarations (Phase 4b).
//
// A lightweight header other engine headers include to NAME the risk spine types
// without pulling in their full definitions (and the factor exposure builder,
// factored covariance model, and portfolio optimizer machinery behind them).
// Keeping the forward set here means a header that only passes a
// `risk::FactorModel*` or a `risk::PortfolioOptimizer&` around does not
// transitively include the covariance factorization, Ledoit-Wolf factor shrinkage,
// or the turnover-penalized QP solver.
//
// Full definitions live in (added per phase unit):
//   risk/exposures.hpp     — StyleFactor, FactorModelConfig, exposure builder     (P4-6)
//   risk/factor_model.hpp  — FactorModel, FactorModelBuilder                      (P4-7)
//   risk/optimizer.hpp     — OptimizerConfig, PortfolioOptimizer                  (P4-9)
//   risk/capacity.hpp      — CapacityPoint, capacity_curve                        (P4-10)
//
// NOTE: StyleFactor is a scoped enum with an explicit underlying type (atx::u8).
// It is forward-declared here so callers that store or pass these values by type
// can include only this header rather than the full definition headers.

#include "atx/core/types.hpp" // atx::u8 (needed for enum underlying types)

namespace atx::engine::risk {

// =====================================================================
//  Scoped enums — forward declarations with explicit underlying type
// =====================================================================

// Barra-style factor identifier for a style factor exposure column.
// Enumerators: Size (ln cap), Momentum, Volatility, Beta, Liquidity.
// Full definition in risk/exposures.hpp (P4-6).
enum class StyleFactor : atx::u8;

// =====================================================================
//  Factor exposure config + builder (P4-6)
// =====================================================================

// Configuration governing which style factor columns are computed and which
// optional external inputs (cap span, group_map) are provided.
// Full definition in risk/exposures.hpp (P4-6).
struct FactorModelConfig;

// =====================================================================
//  Factor covariance model — V = XFXᵀ + D (P4-7)
// =====================================================================

// Factored risk model: cross-sectional WLS factor regression producing the
// factor covariance F (Ledoit-Wolf shrunk) and specific variance diagonal D,
// from which the full covariance V = XFXᵀ + D is recovered via Woodbury.
// Full definition in risk/factor_model.hpp (P4-7).
class FactorModel;

// Fits a FactorModel on a trailing window of PanelView returns and exposure
// matrix X; yields a FactorModel ready for apply-side neutralization and
// optimizer input.
// Full definition in risk/factor_model.hpp (P4-7).
class FactorModelBuilder;

// =====================================================================
//  Turnover-penalized risk-aware optimizer (P4-9)
// =====================================================================

// Parameters governing the optimizer (risk-aversion λ, turnover penalty κ,
// weight bounds, maximum leverage).
// Full definition in risk/optimizer.hpp (P4-9).
struct OptimizerConfig;

// Solves max αᵀw − λ wᵀVw − κ‖w − w_prev‖₁ subject to dollar-neutral and
// leverage constraints, using the factored V from FactorModel.
// Full definition in risk/optimizer.hpp (P4-9).
class PortfolioOptimizer;

// =====================================================================
//  Capacity curve (P4-10)
// =====================================================================

// One point on the capacity–return trade-off curve: a (book_size, expected_ic)
// sample drawn by the walk-forward capacity sweep.
// Full definition in risk/capacity.hpp (P4-10).
struct CapacityPoint;

} // namespace atx::engine::risk
