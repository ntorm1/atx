#include <iostream>
#include "strategy_runner.hpp"

int main(int argc, char** argv) {
  return atx::impl::strategy::dispatch(argc, argv, std::cout, std::cerr);
}
