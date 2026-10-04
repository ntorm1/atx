#include <iostream>
#include <string_view>
#include "strategy_risk_model.hpp"

// `risk` verb: atx-risk-v1.1 factor exposures, factor covariance, specific variances and the
// bias harness for a pinned research role + fields set (platform v7 lanes L4, F2, F3); with
// --recipe atx-cov-v1 / --emit-container the atx.cov-container file (P9 COV, K-P9-12).
// `cov-info` and `cov-diff`: inspect and compare atx.cov-container files.
int main(int argc, char** argv) {
  namespace rk = atx::impl::strategy::risk;
  if (argc > 1 && std::string_view{argv[1]} == "risk")
    return rk::dispatch_risk_model(argc - 1, argv + 1, std::cout, std::cerr);
  if (argc > 1 && std::string_view{argv[1]} == "cov-info")
    return rk::dispatch_cov_info(argc - 1, argv + 1, std::cout, std::cerr);
  if (argc > 1 && std::string_view{argv[1]} == "cov-diff")
    return rk::dispatch_cov_diff(argc - 1, argv + 1, std::cout, std::cerr);
  std::cerr << "usage: atx-equity-strategy-risk risk --help | cov-info FILE [--verify full|lazy] | "
               "cov-diff A B [--through SESSION] | cov-diff --legacy DIR\n";
  return 2;
}
