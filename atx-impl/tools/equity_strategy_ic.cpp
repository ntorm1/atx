#include <iostream>
#include <string_view>
#include "strategy_ic_runner.hpp"
namespace {
// `atx-equity-strategy-ic [VERB] OPTIONS...`. A first argument naming a verb below
// routes to it, with argv shifted so the handler sees the verb as argv[0] and its
// options from argv[1]; anything else is the IC runner's own option list (the
// historical CLI, also reachable as the `ic` verb). Add one line per verb.
using VerbMain=int (*)(int,char**,std::ostream&,std::ostream&);
struct Verb { std::string_view name; VerbMain run; };
constexpr Verb verbs[]{
    {"ic",&atx::impl::strategy::dispatch_ic},
};
} // namespace
int main(int argc,char** argv) {
  if (argc>1)
    for (const auto& verb:verbs)
      if (verb.name==argv[1]) return verb.run(argc-1,argv+1,std::cout,std::cerr);
  return atx::impl::strategy::dispatch_ic(argc,argv,std::cout,std::cerr);
}
