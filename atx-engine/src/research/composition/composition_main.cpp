// atx-research-composition: the composition fit on a K-P9-4 factor series (composition_cli.hpp).
#include <iostream>
#include <string_view>
#include <vector>

#include "atx/engine/research/composition/composition_cli.hpp"

int main(int argc, char **argv) {
  std::vector<std::string_view> args;
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return atx::engine::research::composition::research_composition_main(args, std::cout, std::cerr);
}
