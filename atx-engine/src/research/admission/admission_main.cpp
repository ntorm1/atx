// atx-research-admission: the engine's admission screen on a K-P9-4 factor series
// (admission_cli.hpp).
#include <iostream>
#include <string_view>
#include <vector>

#include "atx/engine/research/admission/admission_cli.hpp"

int main(int argc, char **argv) {
  std::vector<std::string_view> args;
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return atx::engine::research::admission::research_admission_main(args, std::cout, std::cerr);
}
