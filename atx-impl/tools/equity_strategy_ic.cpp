#include <iostream>
#include <string_view>
#include "strategy_ic_runner.hpp"
#include "strategy_marginal_ic.hpp"
int main(int argc,char** argv) {
  if (argc>1 && std::string_view{argv[1]}=="marginal")
    return atx::impl::strategy::dispatch_marginal_ic(argc-1,argv+1,std::cout,std::cerr);
  return atx::impl::strategy::dispatch_ic(argc,argv,std::cout,std::cerr);
}
