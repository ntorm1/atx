#pragma once

// The isolated SpiderRock <-> atx-vol translation layer. Stage 3 resolves a
// single ConventionMap on aggregate smoke+tune data; production Mode A uses
// winning_convention() and never exposes a runtime convention flag.

#include <string_view>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "oracle_cohort_reader.hpp"

namespace atx::vol::oracle {

enum class InputModel {
  CurrentSpotSdivYield,
  DiscreteDividendPvSdivYield,
  DiscreteForwardNetCarry,
  DiscreteForwardRateSdivYield,
  DiscreteForwardNetRate,
  DiscreteForwardZeroRates,
  DiscreteDividendPvNetRate,
  DiscreteDividendPvRatePlusSdiv,
};

enum class GreekSource { Delta, Gamma, Theta, Vega, Rho, CarryRho, Volga, Vanna, Charm };

struct ConventionMap {
  InputModel input_model = InputModel::CurrentSpotSdivYield;
  double price_scale = 1.0;
  // Calendar days-to-expiry, used ONLY for the scorecard's 0-7/8-30/31-90/90+
  // banding. SpiderRock's theta day count is an unrelated convention, so the
  // sweep never writes this field: re-bucketing every band as a side effect of
  // a theta unit pick would silently change what the bands mean.
  double days_per_year = 365.0;
  // Day count implied by theta_scale; this is what the receipt reports as
  // `day_count`.
  double theta_days_per_year = 365.0;
  double delta_scale = 1.0;
  double gamma_scale = 1.0;
  double theta_scale = 1.0 / 365.0;
  double vega_scale = 0.01;
  double rho_scale = 0.01;
  double phi_scale = 0.01;
  GreekSource volga_source = GreekSource::Volga;
  double volga_scale = 0.01;
  GreekSource vanna_source = GreekSource::Vanna;
  double vanna_scale = 0.01;
  double delta_decay_scale = 1.0 / 365.0;
};

[[nodiscard]] const ConventionMap &baseline_convention() noexcept;
[[nodiscard]] const ConventionMap &winning_convention() noexcept;
[[nodiscard]] std::string_view input_model_id(InputModel model) noexcept;
[[nodiscard]] std::string_view greek_source_id(GreekSource source) noexcept;

struct EnginePricingInputs {
  double spot = 0.0;
  double strike = 0.0;
  double years = 0.0;
  double sigma = 0.0;
  double rate = 0.0;
  double carry = 0.0;
  Side side = Side::Call;
};

[[nodiscard]] EnginePricingInputs mode_a_inputs(const OracleRow &row,
                                                const ConventionMap &map) noexcept;
[[nodiscard]] EnginePricingInputs mode_a_inputs(const OracleRow &row) noexcept;
[[nodiscard]] double dte_days(double years, const ConventionMap &map) noexcept;
[[nodiscard]] double dte_days(double years) noexcept;
[[nodiscard]] double price_to_oracle_units(double engine_price, const ConventionMap &map) noexcept;
[[nodiscard]] double price_to_oracle_units(double engine_price) noexcept;

struct OracleUnitGreeks {
  double de = 0.0;
  double ga = 0.0;
  double th = 0.0;
  double ve = 0.0;
  double rh = 0.0;
  double ph = 0.0;
  double vo = 0.0;
  double va = 0.0;
  double de_decay = 0.0;
};

[[nodiscard]] OracleUnitGreeks to_oracle_units(const AmericanGreeks &g, double dp_dq,
                                               const ConventionMap &map) noexcept;
[[nodiscard]] OracleUnitGreeks to_oracle_units(const AmericanGreeks &g, double dp_dq) noexcept;

} // namespace atx::vol::oracle
