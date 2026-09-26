#include <iostream>
#include "strategy_ic_runner.hpp"
int main(int argc,char** argv) {
  return atx::impl::strategy::dispatch_ic(argc,argv,std::cout,std::cerr);
}
