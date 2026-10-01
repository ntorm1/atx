#include <iostream>
#include "strategy_mine.hpp"

// `atx-equity-strategy-mine` (platform v8 H-3): one mining campaign on a pinned research role
// (strategy_mine.hpp). Built and self-tested on a synthetic role; a campaign on real data runs
// only under owner decision OD-7.
int main(int argc, char** argv) {
  return atx::impl::strategy::dispatch_mine(argc, argv, std::cout, std::cerr);
}
