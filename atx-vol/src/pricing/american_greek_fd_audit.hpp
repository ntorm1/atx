#pragma once

// Private diagnostic seam. Explicit steps never change public FD defaults and
// define no derivative accuracy budget or adaptive acceptance policy.
#include <array>
#include <cstddef>
#include <limits>
#include <optional>

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol::detail {

struct AlGreekFdSteps {
  double spot = 0.;
  double volatility = 0.;
  double rate = 0.;
  double time = 0.;
};

struct AlGreekFdSample {
  double S = 0., K = 0., T = 0., sigma = 0., r = 0., q = 0.;
  double price = std::numeric_limits<double>::quiet_NaN();
  double internal_boundary = std::numeric_limits<double>::quiet_NaN();
  bool boundary_required = false;
  bool exercised = false;
  bool evaluated = false; // false for a one-sided alias or after an earlier price refusal
};

struct AlGreekFdAudit {
  AlGreekFdSteps steps{}; // actual steps, including the existing volatility clamp
  // Center; S+/-; sigma+/-; r+/-; T+/-; S+sigma+, S+sigma-, S-sigma+, S-sigma-;
  // S+T+, S-T+, S+T-, S-T-. Aliases retain actual model parameters, not a fictitious bump.
  std::array<AlGreekFdSample, 17> samples{};
  std::size_t sample_attempts = 0; // actual evaluator attempts, excluding short-circuited slots
  std::size_t boundary_cold_solves = 0;
  std::size_t boundary_warm_solves = 0;
  std::size_t boundary_price_calls = 0;
  std::size_t public_price_calls = 0; // opaque fallback; not claimed as one boundary solve
  std::size_t european_price_calls = 0;
  std::size_t degenerate_samples = 0;
  bool rho_forward = false;
  bool time_forward = false;
};

// Same memo, arithmetic and per-field admission as american_greeks_fd. Invalid
// finite-difference steps refuse before pricing; audit is always reset. Existing
// near-expiry/rate one-sided rules and volatility-step clamping remain explicit.
// Observation adds no pricing/boundary solve and owns no dynamic storage.
[[nodiscard]] Result<AmericanGreeks> audit_american_greeks_fd(
    double S, double K, double T, double sigma, double r, double q, Side side,
    const AlGreekFdSteps &steps, AlGreekFdAudit &audit,
    AmericanMethod method = AmericanMethod::AndersenLake,
    const std::optional<AlOpts> &opts = std::nullopt, bool warm_start = false);

} // namespace atx::vol::detail
