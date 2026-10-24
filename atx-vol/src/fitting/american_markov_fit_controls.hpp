#pragma once

#include "atx/vol/api/fitting/american_markov_fit.hpp"

namespace atx::vol::detail {

// Shared by the fitter and the policy catalog so a profile can never admit a
// search/budget control the fitter would refuse. Every limit is explicit;
// nothing here widens an existing cap.
[[nodiscard]] Status validate_markov_fit_search(const AmericanMarkovFitConfig &config);

} // namespace atx::vol::detail
