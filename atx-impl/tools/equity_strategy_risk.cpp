#include <iostream>
#include <string_view>
#include "strategy_risk_model.hpp"

// `risk` verb: atx-risk-v1 factor exposures, factor covariance, specific variances and the
// bias harness for a pinned research role + fields set (platform v7 lane L4).
int main(int argc, char** argv) {
  if (argc > 1 && std::string_view{argv[1]} == "risk")
    return atx::impl::strategy::risk::dispatch_risk_model(argc - 1, argv + 1, std::cout,
                                                          std::cerr);
  std::cerr << "usage: atx-equity-strategy-risk risk --help\n";
  return 2;
}
