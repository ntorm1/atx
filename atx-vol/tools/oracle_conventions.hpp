#pragma once

// THE SpiderRock convention layer for atx-vol-oracle-bench
// (bench/oracle/CHARTER.md stage 2).
//
// EVERYTHING that maps between SpiderRock's units/semantics and the atx-vol
// engine's lives in THIS one translation unit and nowhere else. Charter stage
// 3 (iteration 0, convention resolution) will rewrite oracle_conventions.cpp
// against the measured round-trip residual; the rest of the tool depends only
// on the three function contracts below and must not care.
//
// The stage-2 mappings in the .cpp are INITIAL HYPOTHESES (documented per
// mapping there), good enough to stand the tool up — they are exactly what
// iteration 0 exists to falsify or confirm.

#include "atx/vol/api/core/types.hpp"      // Side
#include "atx/vol/api/pricing/american.hpp" // AmericanGreeks
#include "oracle_cohort_reader.hpp"         // OracleRow

namespace atx::vol::oracle {

// Engine-facing pricing inputs assembled from SpiderRock's OWN row inputs
// (Mode A: uPrc, rate, sdiv, ddiv, years; vol = srVol).
struct EnginePricingInputs {
  double spot = 0.0;
  double strike = 0.0;
  double years = 0.0;
  double sigma = 0.0;
  double rate = 0.0;
  double carry = 0.0; // the continuous yield q handed to the American pricer
  Side side = Side::Call;
};

[[nodiscard]] EnginePricingInputs mode_a_inputs(const OracleRow &row) noexcept;

// SpiderRock `years` -> calendar days-to-expiry for dte-band assignment.
[[nodiscard]] double dte_days(double years) noexcept;

// Engine premium -> SpiderRock srPrc units.
[[nodiscard]] double price_to_oracle_units(double engine_price) noexcept;

// Engine greeks -> SpiderRock's de/ga/th/ve/rh/ph/vo/va/deDecay units.
// `dp_dq` is the carry sensitivity from american_carry_greeks_* (the ph
// hypothesis); pass NaN when unavailable — ph is then NaN and the caller
// skips that one metric.
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

[[nodiscard]] OracleUnitGreeks to_oracle_units(const AmericanGreeks &g, double dp_dq) noexcept;

} // namespace atx::vol::oracle
