#pragma once
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include <span>

namespace atx::engine::risk {
enum class CostedSolveRule : atx::u8 { AugmentedConeV1 = 1, FactorProxV2 = 2 };

// All coefficients are NAV-return objective units; w_prev is the actually held
// marked book. Borrow is an asymmetric holding fee, not evidence of a locate.
struct TradeCostTerms {
  std::span<const atx::f64> kappa_lin;
  std::span<const atx::f64> c_three_halves;
  std::span<const atx::f64> borrow_fee;
  std::span<const atx::f64> locate_cap;
  std::span<const atx::f64> w_prev;
  std::span<const atx::u8> untradeable{};
  std::span<const atx::f64> max_trade{}; // finite <=0 is a hard no-trade pin; +inf uncapped
  [[nodiscard]] bool active() const noexcept;
};
[[nodiscard]] atx::core::Status validate_cost_terms(const TradeCostTerms &, atx::usize instruments);

// prox of kappa*|u| + impact*|u|^(3/2), with quadratic rho/2*(u-v)^2.
// Used by the checked V2 solver after validating all scalar inputs.
namespace cost_detail {
[[nodiscard]] atx::f64 trade_prox(atx::f64 v, atx::f64 kappa, atx::f64 impact,
                                  atx::f64 rho) noexcept;
[[nodiscard]] atx::core::Status project_holdings(std::span<const atx::f64> target,
                                                 std::span<const atx::f64> lower,
                                                 std::span<const atx::f64> upper,
                                                 std::span<const atx::f64> borrow_fee, atx::f64 rho,
                                                 atx::f64 gross_budget, std::span<atx::f64> output);
[[nodiscard]] atx::core::Status project_trades(std::span<const atx::f64> target,
                                               std::span<const atx::f64> previous,
                                               const TradeCostTerms &terms, atx::f64 scalar_kappa,
                                               atx::f64 rho, atx::f64 turnover_budget,
                                               std::span<atx::f64> output);
} // namespace cost_detail
[[nodiscard]] atx::core::Result<atx::f64> prox_trade_cost(atx::f64 v, atx::f64 kappa,
                                                          atx::f64 impact, atx::f64 rho);
} // namespace atx::engine::risk
