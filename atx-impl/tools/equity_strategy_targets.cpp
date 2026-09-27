#include <iostream>
#include "strategy_target_replay.hpp"

int main(int argc, char** argv) {
  return atx::impl::strategy::dispatch_target_replay(argc, argv, std::cout, std::cerr);
}
